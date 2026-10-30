#include "periph/gdma_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kFinInt = 2, kHalfInt = 4, kRepeat = 8, kSrcInc = 0x100, kDstInc = 0x200;
constexpr uint32_t kSrcLoop = 0x400, kDstLoop = 0x800;

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

uint32_t address32(uint64_t address, const char *reason) {
    if (address > 0xFFFFFFFFULL) throw BusError(static_cast<uint32_t>(address), reason);
    return static_cast<uint32_t>(address);
}

} // namespace

// ---- Mover ----

GdmaEngine::Mover::Mover(BusMemory                   &memory,
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

uint64_t GdmaEngine::Mover::run(std::optional<uint64_t> max_writes) {
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

uint32_t GdmaEngine::Mover::remain() const {
    return length_ - static_cast<uint32_t>(std::min<uint64_t>(length_, writes * dst_bytes_));
}

uint64_t GdmaEngine::Mover::half_writes() const { return (length_ + 2 * dst_bytes_ - 1) / (2 * dst_bytes_); }

// ---- GdmaEngine ----

GdmaEngine::GdmaEngine(
    const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz, unsigned channels, Encoding encoding)
    : clock_(clock), irq_(std::move(irq)), memory_(memory), bus_hz_(bus_hz), count_(std::min(channels, kMaxChannels)),
      encoding_(std::move(encoding)) {}

uint32_t GdmaEngine::src_request(uint32_t mux) const {
    return (mux >> encoding_.src_shift) & ((1u << encoding_.request_bits) - 1);
}

uint32_t GdmaEngine::dst_request(uint32_t mux) const {
    return (mux >> encoding_.dst_shift) & ((1u << encoding_.request_bits) - 1);
}

bool GdmaEngine::is_memory(uint32_t request) const {
    const auto &memory = encoding_.memory_requests;
    return std::find(memory.begin(), memory.end(), request) != memory.end();
}

unsigned GdmaEngine::priority(uint32_t config) const { return (config >> 12) & ((1u << encoding_.priority_bits) - 1); }

std::string GdmaEngine::endpoint(uint32_t request, uint32_t address) const {
    if (is_memory(request)) return fmt("DTCM 0x%08X", address);
    const char *name = encoding_.request_name ? encoding_.request_name(request) : nullptr;
    return name ? fmt("%s 0x%08X", name, address) : fmt("req%u 0x%08X", request, address);
}

std::shared_ptr<RequestLine> GdmaEngine::request_line(unsigned n) {
    auto line = std::make_shared<RequestLine>();
    ports_[n] = line;
    return line;
}

void GdmaEngine::reset() {
    for (Channel &c : channels_) c = Channel{};
    fin_ = half_ = 0;
    prio_        = false;
    advanced_ns_ = clock_.now_ns();
    update_irq();
}

void GdmaEngine::sync() {
    advance(clock_.now_ns());
    update_irq();
}

void GdmaEngine::settle() {
    // Fixed priority never grants a channel of priority 0, and it stays so after
    // a switch back to round-robin.
    for (Channel &c : channels_) {
        if (c.running && prio_ && priority(c.config) == 0) c.stuck = true;
    }
    update_irq();
}

uint32_t GdmaEngine::config(unsigned channel) const {
    const Channel &c = channels_[channel];
    return (c.config & ~1u) | (c.running ? 1u : 0u);
}

uint32_t GdmaEngine::remain(unsigned channel) const {
    const Channel &c = channels_[channel];
    return c.mover ? c.mover->remain() : 0;
}

uint32_t GdmaEngine::src_rd_addr(unsigned channel) const {
    const Channel &c = channels_[channel];
    return c.mover ? static_cast<uint32_t>(c.mover->src) : c.words.src;
}

uint32_t GdmaEngine::dst_wr_addr(unsigned channel) const {
    const Channel &c = channels_[channel];
    return c.mover ? static_cast<uint32_t>(c.mover->dst) : c.words.dst;
}

bool GdmaEngine::stuck() const {
    return std::any_of(channels_.begin(), channels_.end(), [](const Channel &c) { return c.running && c.stuck; });
}

void GdmaEngine::tick(uint64_t now_ns) {
    advance(now_ns);
    for (unsigned i = 0; i < count_; ++i) {
        Channel &c = channels_[i];
        if (c.running && c.paced) run_paced(i, c, now_ns);
    }
    update_irq();
}

std::optional<uint64_t> GdmaEngine::next_event_ns() const {
    std::optional<uint64_t> next;
    auto                    note = [&](uint64_t t) { next = next ? std::min(*next, t) : t; };
    const auto              rate = rates({});
    for (unsigned i = 0; i < count_; ++i) {
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
void GdmaEngine::advance(uint64_t now_ns) {
    if (now_ns <= advanced_ns_) return;
    double budget = static_cast<double>(now_ns - advanced_ns_) * bus_hz_ / 1e8; // tenths of a bus cycle
    advanced_ns_  = now_ns;
    std::array<bool, kMaxChannels> held{};
    while (budget > 0) {
        const auto rate = rates(held);
        double     step = budget;
        bool       any  = false;
        for (unsigned i = 0; i < count_; ++i) {
            if (rate[i] <= 0) continue;
            any  = true;
            step = std::min(step, std::max(0.0, (target(channels_[i]) - channels_[i].progress) / rate[i]));
        }
        if (!any) break;
        for (unsigned i = 0; i < count_; ++i) {
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
std::array<double, GdmaEngine::kMaxChannels> GdmaEngine::rates(const std::array<bool, kMaxChannels> &held) const {
    std::array<double, kMaxChannels> rate{};
    std::vector<unsigned>            active;
    for (unsigned i = 0; i < count_; ++i) {
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

double GdmaEngine::target(const Channel &channel) const {
    return static_cast<double>(channel.half_pending ? channel.half_at : channel.transactions);
}

bool GdmaEngine::has_event(const Channel &channel) const {
    return channel.half_pending || !(channel.config & kRepeat) || (channel.config & kFinInt);
}

bool GdmaEngine::move(unsigned index, Channel &channel) {
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

bool GdmaEngine::reach_target(unsigned index, Channel &channel) {
    if (channel.half_pending) {
        half_finished(index, channel);
        return true;
    }
    end_cycle(index, channel);
    return (channel.config & kFinInt) != 0;
}

void GdmaEngine::run_paced(unsigned index, Channel &channel, uint64_t now_ns) {
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

std::string GdmaEngine::describe(const Channel &channel) const {
    const uint32_t cfg = channel.config, mux = channel.words.mux;
    const auto    &loops  = channel.words.loops;
    const uint32_t length = (cfg >> 16) + 1;
    const unsigned sdw = (cfg >> 4) & 3, ddw = (cfg >> 6) & 3;
    std::string    events;
    if (cfg & kFinInt) events += "fin";
    if (cfg & kHalfInt) events += std::string(events.empty() ? "" : "+") + "h_fin";
    std::string notes;
    if (cfg & kRepeat) notes += ", repeat";
    if (cfg & kSrcLoop) notes += fmt(", src loop 0x%08X..0x%08X", loops[3], loops[2]);
    if (cfg & kDstLoop) notes += fmt(", dst loop 0x%08X..0x%08X", loops[1], loops[0]);
    const uint32_t rd = (mux >> 12) & 0xF, wr = (mux >> 16) & 0xF;
    if (rd || wr) notes += fmt(", interval r%u/w%u", rd, wr);
    return fmt("%s -> %s, %u bytes (%u writes), src %u-bit %s, dst %u-bit %s, irq: %s%s",
               endpoint(src_request(mux), channel.words.src).c_str(),
               endpoint(dst_request(mux), channel.words.dst).c_str(),
               length,
               (length + (1u << ddw) - 1) >> ddw,
               8u << sdw,
               (cfg & kSrcInc) ? "+4" : "fixed",
               8u << ddw,
               (cfg & kDstInc) ? "+4" : "fixed",
               events.empty() ? "none" : events.c_str(),
               notes.c_str());
}

void GdmaEngine::write_config(unsigned index, uint32_t value) {
    Channel   &channel = channels_[index];
    const bool enable  = (value & 1) != 0;
    channel.config     = value;
    if (enable && !channel.running) {
        start(index, channel);
    } else if (!enable && channel.running) {
        channel.running = false;
        channel.stalled = false;
        channel.stuck   = false;
        log().log(Level::Debug, fmt("ch%u stopped before it finished", index));
    } else if (value == 0) {
        log().log(Level::Debug, fmt("ch%u config cleared", index));
    } else if (!enable && channel.words.src != 0 && channel.words.dst != 0) {
        log().log(Level::Debug, fmt("ch%u config %s", index, describe(channel).c_str()));
    }
}

void GdmaEngine::start(unsigned index, Channel &channel) {
    const uint32_t src_req = src_request(channel.words.mux), dst_req = dst_request(channel.words.mux);

    std::shared_ptr<RequestLine> port;
    if (!is_memory(src_req) && is_memory(dst_req)) {
        auto it = ports_.find(src_req);
        if (it != ports_.end()) port = it->second;
    }
    channel.paced   = port != nullptr;
    channel.port    = port;
    channel.running = true;
    channel.stuck   = false;
    channel.period  = period(channel.words.mux);
    begin_cycle(channel, channel.words.src, channel.words.dst);

    const uint64_t duration = channel.transactions * channel.period * 100'000'000ULL / bus_hz_;
    log().log(Level::Info, fmt("ch%u start: %s, est %s", index, describe(channel).c_str(), human(duration).c_str()));
    if (channel.paced) {
        channel.word_ns = std::max<uint64_t>(duration / std::max<uint64_t>(channel.mover->writes_total, 1), 1);
        channel.last_ns = clock_.now_ns();
    }
    channel.stalled = !channel.paced && (!is_memory(src_req) || !is_memory(dst_req));
    if (channel.stalled) {
        for (auto [side, req] : {
                 std::pair{"source",      src_req},
                 std::pair{"destination", dst_req}
        }) {
            if (is_memory(req)) continue;
            const char *name = encoding_.request_name ? encoding_.request_name(req) : nullptr;
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
void GdmaEngine::begin_cycle(Channel &channel, uint64_t src, uint64_t dst) {
    const uint32_t        cfg   = channel.config;
    const auto           &loops = channel.words.loops;
    std::optional<Window> src_window, dst_window;
    if ((cfg & kSrcLoop) && loops[2] != 0) src_window = Window{loops[3], loops[2]};
    if ((cfg & kDstLoop) && loops[0] != 0) dst_window = Window{loops[1], loops[0]};
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

void GdmaEngine::half_finished(unsigned index, Channel &channel) {
    channel.half_pending = false;
    half_ |= 1u << index;
    channel.half_count = (channel.half_count + 1) & 0xF;
    log().log(Level::Debug, fmt("ch%u half-finish flag set", index));
}

// Repeat mode starts the next cycle where this one ended.
void GdmaEngine::end_cycle(unsigned index, Channel &channel) {
    flag_finished(index, channel);
    if (channel.config & kRepeat) {
        begin_cycle(channel, channel.mover->src, channel.mover->dst);
        return;
    }
    channel.running = false;
    log().log(Level::Debug, fmt("ch%u done: %u bytes copied", index, (channel.config >> 16) + 1));
}

void GdmaEngine::flag_finished(unsigned index, Channel &channel) {
    if (channel.config & kFinInt) {
        fin_ |= 1u << index;
        channel.fin_count = (channel.fin_count + 1) & 0xF;
        log().log(Level::Debug, fmt("ch%u finish flag set", index));
    }
}

void GdmaEngine::abort(unsigned index, Channel &channel, const BusError &err) {
    log().log(Level::Error,
              fmt("ch%u aborted: %s (src 0x%08X -> dst 0x%08X)",
                  index,
                  err.what(),
                  static_cast<uint32_t>(channel.mover->src),
                  static_cast<uint32_t>(channel.mover->dst)));
    channel.running = false;
}

void GdmaEngine::acknowledge(uint32_t finished, uint32_t half_finished) {
    const uint32_t mask = (1u << count_) - 1;
    const uint32_t fin = finished & mask, half = half_finished & mask;
    for (unsigned i = 0; i < count_; ++i) {
        if (((fin >> i) & 1) && ((fin_ >> i) & 1)) log().log(Level::Debug, fmt("ch%u fin flag cleared by software", i));
        if (((half >> i) & 1) && ((half_ >> i) & 1))
            log().log(Level::Debug, fmt("ch%u half-fin flag cleared by software", i));
    }
    fin_ &= ~fin;
    half_ &= ~half;
}

void GdmaEngine::update_irq() {
    if (irq_) irq_(fin_ != 0 || half_ != 0);
}

} // namespace bk
