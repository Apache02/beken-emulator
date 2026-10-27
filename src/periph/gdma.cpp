#include "periph/gdma.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kChStride = 0x20;
constexpr uint32_t kStatus = 0xC0, kPrioMode = 0xDC, kIntStatus = 0xE0;
constexpr uint32_t kSrcPause = 0x100, kDstPause = 0x120, kDstWr = 0x160;
constexpr uint32_t kConfig = 0x00, kDstStart = 0x04, kSrcStart = 0x08, kMux = 0x1C;
constexpr uint32_t kFinInt = 2, kHalfInt = 4, kRepeat = 8, kSrcInc = 0x100, kDstInc = 0x200;
constexpr uint32_t kSrcLoop = 0x400, kDstLoop = 0x800;
// int_status reads these reserved bits while a channel is stuck at fixed priority 0.
constexpr uint32_t kStuckBits = 0x00050000;

// Costs in tenths of a bus cycle. A channel alone moves one word per 7.2
// cycles; all channels together get one word per 5 cycles (six 8 KB copies
// finish together after about 510 us, two 1 KB copies take 22 us against 16 us
// alone).
constexpr uint64_t kAloneTenths = 72;
constexpr double   kBusTenths   = 50;
constexpr double   kEpsilon     = 1e-6;

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

// The cost of one transaction when the channel has the bus alone: 7.2 bus
// cycles; from a pacing interval of 6 up the period is interval + 2 cycles.
uint64_t period(uint32_t mux) {
    const uint32_t interval = std::max((mux >> 12) & 0xF, (mux >> 16) & 0xF);
    return interval >= 6 ? std::max<uint64_t>(kAloneTenths, (interval + 2) * 10) : kAloneTenths;
}

unsigned priority(uint32_t config) { return (config >> 12) & 3; }

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
                   std::optional<Window>        src_window,
                   std::optional<Window>        dst_window,
                   std::shared_ptr<RequestLine> port)
    : src(src_addr), dst(dst_addr), memory_(memory), port_(std::move(port)), length_(length),
      src_bytes_(uint64_t{1} << src_width), dst_bytes_(uint64_t{1} << dst_width), src_inc_(src_inc), dst_inc_(dst_inc),
      src_window_(src_window), dst_window_(dst_window) {
    writes_total = (length + dst_bytes_ - 1) / dst_bytes_;
    reads_total  = (length + src_bytes_ - 1) / src_bytes_;
}

uint64_t Gdma::Mover::run(std::optional<uint64_t> max_writes) {
    auto next = [](uint64_t &address, bool inc, const std::optional<Window> &window) {
        if (!inc) return;
        address += 4;
        if (window && address >= window->end) address = window->start;
    };
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
            next(src, src_inc_, src_window_);
        }
        std::vector<uint8_t> chunk(dst_bytes_, 0);
        const size_t         have = std::min<size_t>(accumulator_.size(), dst_bytes_);
        std::copy(accumulator_.begin(), accumulator_.begin() + have, chunk.begin());
        accumulator_.erase(accumulator_.begin(), accumulator_.begin() + have);
        memory_.write(address32(dst, "nothing writable there"), chunk);
        next(dst, dst_inc_, dst_window_);
        ++writes;
        ++done;
    }
    return done;
}

uint32_t Gdma::Mover::remain() const {
    return length_ - static_cast<uint32_t>(std::min<uint64_t>(length_, writes * dst_bytes_));
}

uint64_t Gdma::Mover::half_writes() const { return (length_ + 2 * dst_bytes_ - 1) / (2 * dst_bytes_); }

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
    advanced_ns_         = clock_.now_ns();
    update_irq();
}

uint32_t Gdma::read(const std::string &, uint32_t offset, uint32_t) {
    advance(clock_.now_ns());
    update_irq();
    const uint32_t index = offset / kChStride;
    if (index < kChannels) return read_channel(channels_[index], offset % kChStride);
    if (offset >= kStatus && offset < kStatus + 4 * kChannels) return status((offset - kStatus) / 4);
    if (offset == kPrioMode) return prio_;
    if (offset == kIntStatus) {
        const bool stuck =
            std::any_of(channels_.begin(), channels_.end(), [](const Channel &c) { return c.running && c.stuck; });
        return fin_ | half_ << 8 | (stuck ? kStuckBits : 0);
    }
    if (offset >= kSrcPause && offset < kSrcPause + 4 * kChannels) return channels_[(offset - kSrcPause) / 4].pause[0];
    if (offset >= kDstPause && offset < kDstPause + 4 * kChannels) return channels_[(offset - kDstPause) / 4].pause[1];
    if (offset >= kDstWr && offset < kDstWr + 4 * kChannels) return dst_wr_addr((offset - kDstWr) / 4);
    return 0;
}

void Gdma::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    advance(clock_.now_ns());
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
    // Fixed priority never grants a channel of priority 0, and it stays so after
    // a switch back to round-robin.
    for (Channel &c : channels_) {
        if (c.running && prio_ && priority(c.config) == 0) c.stuck = true;
    }
    update_irq();
}

void Gdma::tick(uint64_t now_ns) {
    advance(now_ns);
    for (unsigned i = 0; i < kChannels; ++i) {
        Channel &c = channels_[i];
        if (c.running && c.paced) run_paced(i, c, now_ns);
    }
    update_irq();
}

std::optional<uint64_t> Gdma::next_event_ns() const {
    std::optional<uint64_t> next;
    auto                    note = [&](uint64_t t) { next = next ? std::min(*next, t) : t; };
    const auto              rate = rates({});
    for (unsigned i = 0; i < kChannels; ++i) {
        const Channel &c = channels_[i];
        if (c.running && c.paced) {
            if (c.port->level()) note(c.last_ns + c.word_ns);
        } else if (rate[i] > 0 && has_event(c)) {
            const double ns = (target(c) - c.progress) / rate[i] * 1e8 / bus_hz_;
            note(advanced_ns_ + std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(ns - kEpsilon))));
        }
    }
    return next;
}

// Moves every memory-to-memory channel up to `now_ns`, in steps that end at
// the next event of any channel, since an event can change the bus shares.
void Gdma::advance(uint64_t now_ns) {
    if (now_ns <= advanced_ns_) return;
    double budget = static_cast<double>(now_ns - advanced_ns_) * bus_hz_ / 1e8; // tenths of a bus cycle
    advanced_ns_  = now_ns;
    std::array<bool, kChannels> held{};
    while (budget > 0) {
        const auto rate = rates(held);
        double     step = budget;
        bool       any  = false;
        for (unsigned i = 0; i < kChannels; ++i) {
            if (rate[i] <= 0) continue;
            any  = true;
            step = std::min(step, std::max(0.0, (target(channels_[i]) - channels_[i].progress) / rate[i]));
        }
        if (!any) break;
        for (unsigned i = 0; i < kChannels; ++i) {
            if (rate[i] <= 0) continue;
            Channel     &c     = channels_[i];
            const double end   = target(c);
            c.progress         = std::min(end, c.progress + rate[i] * step);
            const bool reached = end - c.progress < kEpsilon;
            if (reached) c.progress = end;
            if (!move(i, c)) continue;
            if (reached && reach_target(i, c)) held[i] = true;
        }
        budget -= step;
    }
}

// Words per tenth of a bus cycle for each channel that competes for the bus.
std::array<double, Gdma::kChannels> Gdma::rates(const std::array<bool, kChannels> &held) const {
    std::array<double, kChannels> rate{};
    std::vector<unsigned>         active;
    for (unsigned i = 0; i < kChannels; ++i) {
        const Channel &c = channels_[i];
        if (c.running && !c.stalled && !c.paced && !c.stuck && !held[i]) active.push_back(i);
    }
    double bus = 1.0 / kBusTenths;
    if (prio_) {
        std::sort(active.begin(), active.end(), [&](unsigned a, unsigned b) {
            const unsigned pa = priority(channels_[a].config), pb = priority(channels_[b].config);
            return pa != pb ? pa > pb : a > b;
        });
        for (unsigned i : active) {
            rate[i] = std::min(1.0 / channels_[i].period, std::max(0.0, bus));
            bus -= rate[i];
        }
    } else {
        // Even shares; a channel slower than its share leaves the rest to the others.
        std::sort(active.begin(), active.end(), [&](unsigned a, unsigned b) {
            return channels_[a].period > channels_[b].period;
        });
        size_t left = active.size();
        for (unsigned i : active) {
            rate[i] = std::min(1.0 / channels_[i].period, std::max(0.0, bus) / static_cast<double>(left--));
            bus -= rate[i];
        }
    }
    // What rounding leaves of a used-up bus is no share.
    for (double &r : rate) {
        if (r < kEpsilon * kEpsilon) r = 0;
    }
    return rate;
}

double Gdma::target(const Channel &channel) const {
    return static_cast<double>(channel.half_pending ? channel.half_at : channel.transactions);
}

bool Gdma::has_event(const Channel &channel) const {
    return channel.half_pending || !(channel.config & kRepeat) || (channel.config & kFinInt);
}

bool Gdma::move(unsigned index, Channel &channel) {
    Mover         &mover = *channel.mover;
    const uint64_t done  = static_cast<uint64_t>(channel.progress + kEpsilon);
    const uint64_t want =
        done >= channel.transactions ? mover.writes_total : done * mover.writes_total / channel.transactions;
    try {
        if (want > mover.writes) mover.run(want - mover.writes);
    } catch (const BusError &err) {
        abort(index, channel, err);
        return false;
    }
    return true;
}

bool Gdma::reach_target(unsigned index, Channel &channel) {
    if (channel.half_pending) {
        half_finished(index, channel);
        return true;
    }
    end_cycle(index, channel);
    return (channel.config & kFinInt) != 0;
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
        abort(index, channel, err);
        return;
    }
    channel.last_ns = done < allowed ? now_ns : channel.last_ns + done * channel.word_ns;
    if (channel.half_pending && mover.writes >= mover.half_writes()) half_finished(index, channel);
    if (mover.done()) end_cycle(index, channel);
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
    if (cfg & kFinInt) events += "fin";
    if (cfg & kHalfInt) events += std::string(events.empty() ? "" : "+") + "h_fin";
    std::string notes;
    if (cfg & kRepeat) notes += ", repeat";
    if (cfg & kSrcLoop) notes += ", src loop";
    if (cfg & kDstLoop) notes += ", dst loop";
    const uint32_t rd = (mux >> 12) & 0xF, wr = (mux >> 16) & 0xF;
    if (rd || wr) notes += fmt(", interval r%u/w%u", rd, wr);
    return fmt("%s -> %s, %u bytes (%u writes), src %u-bit %s, dst %u-bit %s, irq: %s%s",
               endpoint(mux & 0xF, src).c_str(),
               endpoint((mux >> 4) & 0xF, dst).c_str(),
               length,
               (length + (1u << ddw) - 1) >> ddw,
               8u << sdw,
               (cfg & kSrcInc) ? "+4" : "fixed",
               8u << ddw,
               (cfg & kDstInc) ? "+4" : "fixed",
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
        channel.stuck   = false;
        log().log(Level::Debug, fmt("ch%u stopped before it finished", index));
    } else if (value == 0) {
        log().log(Level::Debug, fmt("ch%u config cleared", index));
    } else if (!enable && channel.src != 0 && channel.dst != 0) {
        log().log(
            Level::Debug,
            fmt("ch%u config %s", index, describe(channel.config, channel.mux, channel.src, channel.dst).c_str()));
    }
}

void Gdma::start(unsigned index, Channel &channel) {
    const uint32_t cfg     = channel.config;
    const uint32_t src_req = channel.mux & 0xF, dst_req = (channel.mux >> 4) & 0xF;

    std::shared_ptr<RequestLine> port;
    if (src_req && !dst_req) {
        auto it = ports_.find(src_req);
        if (it != ports_.end()) port = it->second;
    }
    channel.paced   = port != nullptr;
    channel.port    = port;
    channel.running = true;
    channel.stuck   = false;
    channel.period  = period(channel.mux);
    begin_cycle(channel, channel.src, channel.dst);

    const uint64_t duration = channel.transactions * channel.period * 100'000'000ULL / bus_hz_;
    log().log(Level::Info,
              fmt("ch%u start: %s, est %s",
                  index,
                  describe(cfg, channel.mux, channel.src, channel.dst).c_str(),
                  human(duration).c_str()));
    if (channel.paced) {
        channel.word_ns = std::max<uint64_t>(duration / std::max<uint64_t>(channel.mover->writes_total, 1), 1);
        channel.last_ns = clock_.now_ns();
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
}

// A cycle moves transfer_length + 1 bytes from `src` to `dst` with the
// channel's current widths, increments and loop windows (a window whose end
// register is 0 is off).
void Gdma::begin_cycle(Channel &channel, uint64_t src, uint64_t dst) {
    const uint32_t        cfg = channel.config;
    std::optional<Window> src_window, dst_window;
    if ((cfg & kSrcLoop) && channel.loops[2] != 0) src_window = Window{channel.loops[3], channel.loops[2]};
    if ((cfg & kDstLoop) && channel.loops[0] != 0) dst_window = Window{channel.loops[1], channel.loops[0]};
    channel.mover        = std::make_unique<Mover>(memory_,
                                            src,
                                            dst,
                                            (cfg >> 16) + 1,
                                            (cfg >> 4) & 3,
                                            (cfg >> 6) & 3,
                                            (cfg & kSrcInc) != 0,
                                            (cfg & kDstInc) != 0,
                                            src_window,
                                            dst_window,
                                            channel.paced ? channel.port : nullptr);
    const Mover &mover   = *channel.mover;
    channel.transactions = std::max(mover.writes_total, mover.reads_total);
    channel.half_at      = (mover.half_writes() * channel.transactions + mover.writes_total - 1) / mover.writes_total;
    channel.progress     = 0;
    channel.half_pending = (cfg & kHalfInt) != 0;
}

void Gdma::half_finished(unsigned index, Channel &channel) {
    channel.half_pending = false;
    half_ |= 1u << index;
    channel.half_count = (channel.half_count + 1) & 0xF;
    log().log(Level::Debug, fmt("ch%u half-finish flag set", index));
}

// Repeat mode starts the next cycle where this one ended.
void Gdma::end_cycle(unsigned index, Channel &channel) {
    flag_finished(index, channel);
    if (channel.config & kRepeat) {
        begin_cycle(channel, channel.mover->src, channel.mover->dst);
        return;
    }
    channel.running = false;
    log().log(Level::Debug, fmt("ch%u done: %u bytes copied", index, (channel.config >> 16) + 1));
}

void Gdma::flag_finished(unsigned index, Channel &channel) {
    if (channel.config & kFinInt) {
        fin_ |= 1u << index;
        channel.fin_count = (channel.fin_count + 1) & 0xF;
        log().log(Level::Debug, fmt("ch%u finish flag set", index));
    }
}

void Gdma::abort(unsigned index, Channel &channel, const BusError &err) {
    log().log(Level::Error,
              fmt("ch%u aborted: %s (src 0x%08X -> dst 0x%08X)",
                  index,
                  err.what(),
                  static_cast<uint32_t>(channel.mover->src),
                  static_cast<uint32_t>(channel.mover->dst)));
    channel.running = false;
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
}

uint32_t Gdma::status(unsigned index) const {
    const Channel &channel = channels_[index];
    const uint32_t remain  = channel.mover ? channel.mover->remain() : 0;
    return (remain & 0x1FFFF) | channel.fin_count << 24 | channel.half_count << 28;
}

uint32_t Gdma::dst_wr_addr(unsigned index) const {
    const Channel &channel = channels_[index];
    return channel.mover ? static_cast<uint32_t>(channel.mover->dst) : channel.dst;
}

void Gdma::update_irq() {
    if (irq_) irq_(fin_ != 0 || half_ != 0);
}

} // namespace bk
