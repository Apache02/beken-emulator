#include "periph/gdma.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kChStride  = 0x20;
constexpr uint32_t kIntCounts = 0xC0, kPrioMode = 0xDC, kIntStatus = 0xE0;
constexpr uint32_t kSrcPause = 0x100, kDstPause = 0x120, kDstWr = 0x160;
constexpr uint32_t kConfig = 0x00, kDstStart = 0x04, kSrcStart = 0x08, kMux = 0x1C;

Logger &log() { return Logger::get("gdma"); }

std::string fmt(const char *format, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char *format, ...) {
    char    buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    return buf;
}

const char *request_name(uint32_t request) {
    static const char *const names[] = {
        "DTCM", "HSSPI", "AUDIO", "SDIO", "UART1", "UART2", "I2S", "GSPI", "JPEG", "PSRAM_V", "PSRAM_A", "reserved"};
    return request < 12 ? names[request] : nullptr;
}

std::string endpoint(uint32_t request, uint32_t address) {
    const char *name = request_name(request);
    return name ? fmt("%s 0x%08X", name, address) : fmt("req%u 0x%08X", request, address);
}

std::string human(uint64_t ns) {
    return ns < 1'000'000 ? fmt("%.1f us", static_cast<double>(ns) / 1000)
                          : fmt("%.2f ms", static_cast<double>(ns) / 1e6);
}

struct Timing {
    uint64_t writes, reads, scaled; // scaled: duration in ns * bus_hz
};

// A word costs 7.2 bus cycles; from a pacing interval of 6 up the period is
// interval + 2 cycles.
Timing timing(uint32_t length, unsigned src_width, unsigned dst_width, uint32_t mux) {
    const uint64_t writes     = (length + (1u << dst_width) - 1) >> dst_width;
    const uint64_t reads      = (length + (1u << src_width) - 1) >> src_width;
    const uint32_t interval   = std::max((mux >> 12) & 0xF, (mux >> 16) & 0xF);
    const uint64_t cycles_x10 = interval >= 6 ? std::max<uint64_t>(72, (interval + 2) * 10) : 72;
    return {writes, reads, std::max(writes, reads) * cycles_x10 * 100'000'000ULL};
}

uint32_t address32(uint64_t address, const char *reason) {
    if (address > 0xFFFFFFFFULL) throw BusError(static_cast<uint32_t>(address), reason);
    return static_cast<uint32_t>(address);
}

} // namespace

// ---- Mover ----

Gdma::Mover::Mover(BusMemory                   &memory,
                   uint64_t                     src_addr,
                   uint64_t                     dst_addr,
                   uint32_t                     length,
                   unsigned                     src_width,
                   unsigned                     dst_width,
                   bool                         src_inc,
                   bool                         dst_inc,
                   std::shared_ptr<RequestLine> port)
    : src(src_addr), dst(dst_addr), memory_(memory), port_(std::move(port)), length_(length),
      src_bytes_(uint64_t{1} << src_width), dst_bytes_(uint64_t{1} << dst_width), src_inc_(src_inc), dst_inc_(dst_inc) {
    writes_total = (length + dst_bytes_ - 1) / dst_bytes_;
}

uint64_t Gdma::Mover::run(std::optional<uint64_t> max_writes) {
    uint64_t done = 0;
    while (!this->done() && (!max_writes || done < *max_writes)) {
        while (accumulator_.size() < dst_bytes_ && taken_ < length_) {
            if (port_ && !port_->level()) return done;
            std::array<uint8_t, 4> word;
            if (port_) {
                word = port_->read();
            } else {
                std::vector<uint8_t> bytes = memory_.read(address32(src, "nothing readable there"), 4);
                std::copy(bytes.begin(), bytes.end(), word.begin());
            }
            const uint64_t take = std::min<uint64_t>(src_bytes_, length_ - taken_);
            accumulator_.insert(accumulator_.end(), word.begin(), word.begin() + std::min<uint64_t>(take, 4));
            taken_ += static_cast<uint32_t>(take);
            ++reads;
            if (src_inc_) src += 4;
        }
        std::vector<uint8_t> chunk(dst_bytes_, 0);
        const size_t         have = std::min<size_t>(accumulator_.size(), dst_bytes_);
        std::copy(accumulator_.begin(), accumulator_.begin() + have, chunk.begin());
        accumulator_.erase(accumulator_.begin(), accumulator_.begin() + have);
        memory_.write(address32(dst, "nothing writable there"), chunk);
        if (dst_inc_) dst += 4;
        ++writes;
        ++done;
    }
    return done;
}

// ---- Gdma ----

Gdma::Gdma(std::string name, const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), memory_(memory), bus_hz_(bus_hz) {}

std::shared_ptr<RequestLine> Gdma::request_line(unsigned n) {
    auto line = std::make_shared<RequestLine>();
    ports_[n] = line;
    return line;
}

void Gdma::reset() {
    for (Channel &c : channels_) c = Channel{};
    fin_ = half_ = prio_ = 0;
    update_irq();
}

uint32_t Gdma::read(const std::string &, uint32_t offset, uint32_t) {
    const uint32_t index = offset / kChStride;
    if (index < kChannels) return read_channel(channels_[index], offset % kChStride);
    if (offset >= kIntCounts && offset < kIntCounts + 4 * kChannels) return int_counts((offset - kIntCounts) / 4);
    if (offset == kPrioMode) return prio_;
    if (offset == kIntStatus) return fin_ | half_ << 8;
    if (offset >= kSrcPause && offset < kSrcPause + 4 * kChannels) return channels_[(offset - kSrcPause) / 4].pause[0];
    if (offset >= kDstPause && offset < kDstPause + 4 * kChannels) return channels_[(offset - kDstPause) / 4].pause[1];
    if (offset >= kDstWr && offset < kDstWr + 4 * kChannels) return dst_wr_addr((offset - kDstWr) / 4);
    return 0;
}

void Gdma::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    const uint32_t index = offset / kChStride;
    if (index < kChannels) {
        write_channel(index, offset % kChStride, value);
    } else if (offset == kPrioMode) {
        prio_ = value & 1;
    } else if (offset == kIntStatus) {
        acknowledge(value);
    } else if (offset >= kSrcPause && offset < kSrcPause + 4 * kChannels) {
        channels_[(offset - kSrcPause) / 4].pause[0] = value;
    } else if (offset >= kDstPause && offset < kDstPause + 4 * kChannels) {
        channels_[(offset - kDstPause) / 4].pause[1] = value;
    }
}

void Gdma::tick(uint64_t now_ns) {
    for (unsigned i = 0; i < kChannels; ++i) {
        Channel &c = channels_[i];
        if (!c.running || c.stalled) continue;
        if (c.paced) {
            run_paced(i, c, now_ns);
            continue;
        }
        if (c.half_pending && now_ns >= c.half_ns) half_finished(i, c);
        if (now_ns >= c.end_ns) finish(i, c);
    }
    update_irq();
}

std::optional<uint64_t> Gdma::next_event_ns() const {
    std::optional<uint64_t> next;
    auto                    note = [&](uint64_t t) { next = next ? std::min(*next, t) : t; };
    for (const Channel &c : channels_) {
        if (c.running && c.paced) {
            if (c.port->level()) note(c.last_ns + c.word_ns);
        } else if (c.running && !c.stalled) {
            note(c.half_pending ? c.half_ns : c.end_ns);
            note(c.end_ns);
        }
    }
    return next;
}

void Gdma::run_paced(unsigned index, Channel &channel, uint64_t now_ns) {
    Mover &mover = *channel.mover;
    if (!channel.port->level()) {
        channel.last_ns = now_ns;
        return;
    }
    const uint64_t allowed = (now_ns - channel.last_ns) / channel.word_ns;
    uint64_t       done    = 0;
    try {
        done = mover.run(allowed);
    } catch (const BusError &err) {
        log().log(Level::Error,
                  fmt("ch%u aborted: %s (-> dst 0x%08X)", index, err.what(), static_cast<uint32_t>(mover.dst)));
        channel.running = false;
        return;
    }
    channel.last_ns = done < allowed ? now_ns : channel.last_ns + done * channel.word_ns;
    if (channel.half_pending && mover.writes * 2 >= mover.writes_total) half_finished(index, channel);
    if (mover.done()) {
        channel.running = false;
        flag_finished(index, channel);
    }
}

uint32_t Gdma::read_channel(const Channel &channel, uint32_t within) const {
    switch (within) {
        case kConfig: return (channel.config & ~1u) | (channel.running ? 1u : 0u);
        case kDstStart: return channel.dst;
        case kSrcStart: return channel.src;
        case kMux: return channel.mux;
        default:
            if (within >= 0x0C && within < 0x1C) return channel.loops[(within - 0x0C) / 4];
            return 0;
    }
}

void Gdma::write_channel(unsigned index, uint32_t within, uint32_t value) {
    Channel &channel = channels_[index];
    switch (within) {
        case kConfig: write_config(index, channel, value); break;
        case kDstStart: channel.dst = value; break;
        case kSrcStart: channel.src = value; break;
        case kMux: channel.mux = value; break;
        default:
            if (within >= 0x0C && within < 0x1C) channel.loops[(within - 0x0C) / 4] = value;
            break;
    }
}

namespace {

std::string describe(uint32_t cfg, uint32_t mux, uint32_t src, uint32_t dst) {
    const uint32_t length = (cfg >> 16) + 1;
    const unsigned sdw = (cfg >> 4) & 3, ddw = (cfg >> 6) & 3;
    std::string    events;
    if (cfg & 2) events += "fin_int";
    if (cfg & 4) events += std::string(events.empty() ? "" : "+") + "half_fin_int";
    std::string notes;
    if (cfg & 8) notes += ", repeat";
    const uint32_t rd = (mux >> 12) & 0xF, wr = (mux >> 16) & 0xF;
    if (rd || wr) notes += fmt(", interval r%u/w%u", rd, wr);
    return fmt("%s -> %s, %u bytes (%u writes), src %u-bit %s, dst %u-bit %s, irq: %s%s",
               endpoint(mux & 0xF, src).c_str(),
               endpoint((mux >> 4) & 0xF, dst).c_str(),
               length,
               (length + (1u << ddw) - 1) >> ddw,
               8u << sdw,
               (cfg & 0x100) ? "+4" : "fixed",
               8u << ddw,
               (cfg & 0x200) ? "+4" : "fixed",
               events.empty() ? "none" : events.c_str(),
               notes.c_str());
}

} // namespace

void Gdma::write_config(unsigned index, Channel &channel, uint32_t value) {
    const bool enable = (value & 1) != 0;
    channel.config    = value;
    if (enable && !channel.running) {
        start(index, channel);
    } else if (!enable && channel.running) {
        channel.running = false;
        channel.stalled = false;
        log().log(Level::Debug, fmt("ch%u stopped before it finished", index));
        update_irq();
    } else if (value == 0) {
        log().log(Level::Debug, fmt("ch%u config cleared", index));
    } else if (!enable) {
        log().log(
            Level::Debug,
            fmt("ch%u config %s", index, describe(channel.config, channel.mux, channel.src, channel.dst).c_str()));
    }
}

void Gdma::start(unsigned index, Channel &channel) {
    const uint32_t cfg    = channel.config;
    const uint32_t length = (cfg >> 16) + 1;
    const unsigned sdw = (cfg >> 4) & 3, ddw = (cfg >> 6) & 3;
    const uint32_t src_req = channel.mux & 0xF, dst_req = (channel.mux >> 4) & 0xF;
    const Timing   t        = timing(length, sdw, ddw, channel.mux);
    const uint64_t now      = clock_.now_ns();
    const uint64_t duration = t.scaled / bus_hz_;
    channel.running         = true;
    channel.writes          = t.writes;
    channel.start_ns        = now;
    channel.end_ns          = now + duration;
    channel.half_ns         = now + duration / 2;
    channel.half_pending    = (cfg & 4) != 0;
    log().log(Level::Debug,
              fmt("ch%u start: %s, est %s",
                  index,
                  describe(cfg, channel.mux, channel.src, channel.dst).c_str(),
                  human(duration).c_str()));

    std::shared_ptr<RequestLine> port;
    if (src_req && !dst_req) {
        auto it = ports_.find(src_req);
        if (it != ports_.end()) port = it->second;
    }
    channel.paced = port != nullptr;
    if (channel.paced) {
        channel.port  = port;
        channel.mover = std::make_unique<Mover>(
            memory_, channel.src, channel.dst, length, sdw, ddw, (cfg & 0x100) != 0, (cfg & 0x200) != 0, port);
        channel.word_ns = std::max<uint64_t>(duration / std::max<uint64_t>(t.writes, 1), 1);
        channel.last_ns = now;
    } else {
        channel.mover.reset();
    }
    channel.stalled = !channel.paced && (src_req || dst_req);
    if (channel.stalled) {
        for (auto [side, req] : {
                 std::pair{"source",      src_req},
                 std::pair{"destination", dst_req}
        }) {
            if (!req) continue;
            const char *name = request_name(req);
            log().log(Level::Warning,
                      fmt("ch%u: %s request line %s is not emulated; the transfer stalls",
                          index,
                          side,
                          name ? name : fmt("0x%X", req).c_str()));
        }
    }
    if (cfg & 8) log().log(Level::Warning, fmt("ch%u: repeat mode is not emulated; the transfer runs once", index));
}

void Gdma::half_finished(unsigned index, Channel &channel) {
    channel.half_pending = false;
    half_ |= 1u << index;
    channel.half_count = (channel.half_count + 1) & 0xF;
    log().log(Level::Debug, fmt("ch%u half-finish flag set", index));
}

void Gdma::finish(unsigned index, Channel &channel) {
    const uint32_t cfg    = channel.config;
    const uint32_t length = (cfg >> 16) + 1;
    channel.running       = false;
    try {
        Mover mover(memory_,
                    channel.src,
                    channel.dst,
                    length,
                    (cfg >> 4) & 3,
                    (cfg >> 6) & 3,
                    (cfg & 0x100) != 0,
                    (cfg & 0x200) != 0);
        mover.run(std::nullopt);
    } catch (const BusError &err) {
        log().log(Level::Error,
                  fmt("ch%u aborted: %s (src 0x%08X -> dst 0x%08X, %u bytes)",
                      index,
                      err.what(),
                      channel.src,
                      channel.dst,
                      length));
        return;
    }
    log().log(Level::Debug, fmt("ch%u done: %u bytes copied", index, length));
    flag_finished(index, channel);
}

void Gdma::flag_finished(unsigned index, Channel &channel) {
    if (channel.config & 2) {
        fin_ |= 1u << index;
        channel.fin_count = (channel.fin_count + 1) & 0xF;
        log().log(Level::Debug, fmt("ch%u finish flag set", index));
    }
}

void Gdma::acknowledge(uint32_t value) {
    const uint32_t fin = value & 0x3F, half = (value >> 8) & 0x3F;
    for (unsigned i = 0; i < kChannels; ++i) {
        if (((fin >> i) & 1) && ((fin_ >> i) & 1)) log().log(Level::Debug, fmt("ch%u fin flag cleared by software", i));
        if (((half >> i) & 1) && ((half_ >> i) & 1))
            log().log(Level::Debug, fmt("ch%u half-fin flag cleared by software", i));
    }
    fin_ &= ~fin;
    half_ &= ~half;
    update_irq();
}

// How many destination writes a deferred running channel has done by now,
// scaled linearly over its transfer time.
uint64_t Gdma::done_writes(const Channel &channel) const {
    const uint64_t span  = std::max<uint64_t>(channel.end_ns - channel.start_ns, 1);
    const uint64_t now   = clock_.now_ns();
    const uint64_t spent = now > channel.start_ns ? std::min(now - channel.start_ns, span) : 0;
    return channel.writes * spent / span;
}

uint32_t Gdma::int_counts(unsigned index) const {
    const Channel &channel = channels_[index];
    uint64_t       remain  = 0;
    if (channel.paced && channel.mover)
        remain = channel.mover->writes_total - channel.mover->writes;
    else if (channel.running)
        remain = channel.writes - done_writes(channel);
    return static_cast<uint32_t>(remain) | channel.fin_count << 24 | channel.half_count << 28;
}

uint32_t Gdma::dst_wr_addr(unsigned index) const {
    const Channel &channel = channels_[index];
    if (channel.paced && channel.mover) return static_cast<uint32_t>(channel.mover->dst);
    if (!channel.running) return channel.dst;
    return channel.dst + ((channel.config & 0x200) ? static_cast<uint32_t>(4 * done_writes(channel)) : 0);
}

void Gdma::update_irq() {
    if (irq_) irq_(fin_ != 0 || half_ != 0);
}

} // namespace bk
