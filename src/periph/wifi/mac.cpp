#include "periph/wifi/mac.h"

#include <algorithm>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

// Core bank.
constexpr uint32_t kVersion = 0x004, kStateCntrl = 0x038, kErrorRecovery = 0x054, kBeaconCntrl1 = 0x064,
                   kKeyWord0 = 0x0AC, kKeyAddrLo = 0x0BC, kKeyAddrHi = 0x0C0, kEncrCntrl = 0x0C4, kKeyRange = 0x0D8,
                   kCounter1 = 0x11C, kCounter2Lo = 0x120, kCounter2Hi = 0x124, kAbsTimer0 = 0x128;
constexpr uint32_t kAbsTimers = 10;
constexpr uint32_t kSwUpdate  = 1u << 31;
constexpr uint64_t kMask48    = (uint64_t{1} << 48) - 1;

// Platform bank.
constexpr uint32_t kDozeCntrl2 = 0x048, kMacCntrl2 = 0x050, kGeneralStatus = 0x06C, kGeneralAck = 0x070,
                   kGeneralEnable = 0x074, kTxrxStatus = 0x078, kTxrxAck = 0x07C, kTxrxEnable = 0x080,
                   kEventSet = 0x084, kEventClear = 0x088, kTimerUnmask = 0x08C, kTsfLo = 0x0A4, kTsfHi = 0x0A8,
                   kAirParam1 = 0x160, kAirParam2 = 0x164, kAirValue = 0x168, kDmaSet = 0x180, kDmaClear = 0x184,
                   kDmaStatus1 = 0x188, kRxHeaderHead = 0x1B8, kRxPayloadHead = 0x1BC;

constexpr uint32_t kUseRxHeaderHead = 1u << 26, kUseRxPayloadHead = 1u << 27;
constexpr int      kRssi = -45;

constexpr uint32_t kGeneralTbtt = 1u << 0, kGeneralIdle = 1u << 2, kGeneralTimers = 1u << 3;
constexpr uint32_t kMaster = 1u << 31, kPending = 0x7FFFFFFF;

constexpr uint32_t kCmdRead = 1u << 31, kCmdWrite = 1u << 30, kCmdSearch = 1u << 29, kNotFound = 1u << 28;
constexpr uint32_t kCmdBits = kCmdRead | kCmdWrite | kCmdSearch;

constexpr uint32_t kProtMask = 0x0000003F, kTxMask = 0x00009FC0, kRxMask = 1u << 16,
                   kTimerMask = (1u << 14) | (1u << 17), kMiscMask = 0x1FFC2000 & ~(kRxMask | kTimerMask);

// The counters of the core bank and the TSF of the platform bank.
bool is_counter(uint32_t word) { return word == kCounter1 || word == kCounter2Lo || word == kCounter2Hi; }
bool is_tsf(uint32_t word) { return word == kTsfLo || word == kTsfHi; }

Logger &log() { return Logger::get("wifi.mac_pl"); }

uint64_t mac_address(std::span<const uint8_t> bytes) {
    uint64_t address = 0;
    for (size_t i = 0; i < 6; ++i) address |= uint64_t{bytes[i]} << (8 * i);
    return address;
}

} // namespace

WifiMac::WifiMac(std::string                name,
                 const Clock               &clock,
                 Signal                     powered,
                 Lines                      lines,
                 Unknown::InstructionSource instruction,
                 BusMemory                 &memory,
                 Level                      bank_log_level)
    : name_(std::move(name)), clock_(clock), powered_(std::move(powered)), lines_(std::move(lines)),
      core_(name_ + "_CORE",
            kCoreSize,
            {
                {kVersion, 0x00001C03}
},
            instruction,
            bank_log_level),
      pl_(name_ + "_PL", kPlSize, {}, instruction, bank_log_level),
      engine_(
          memory,
          [this](std::span<const uint8_t> frame) {
              if (air_) air_->transmit(frame);
          },
          [this](uint32_t bits) { raise_txrx(bits); }) {
    reset();
}

void WifiMac::attach(WifiAir *air) {
    air_ = air;
    if (air_) air_->attach([this](std::vector<uint8_t> frame) { receive(frame); });
}

void WifiMac::receive(std::span<const uint8_t> frame) {
    if (state_ != kActive) return;
    engine_.receive(frame, tsf(), kRssi, station_key_index(frame));
}

void WifiMac::reset() {
    if (air_) air_->reset();
    engine_.reset();
    core_.reset();
    pl_.reset();
    start_      = powered_ ? std::nullopt : std::optional<uint64_t>(0);
    offset_us_  = 0;
    lo_written_ = 0;

    state_ = next_state_ = kIdle;
    general_ = txrx_ = events_ = 0;
    targets_.fill(0);
    armed_      = 0;
    tsf_offset_ = 0;
    last_beacon_.reset();
    keys_.clear();
    soft_reset_pending_ = false;
    levels_.fill(-1);
    update_lines();
}

std::optional<uint64_t> WifiMac::elapsed_ns() {
    if (powered_ && !powered_()) {
        start_.reset();
        offset_us_ = 0;
        return std::nullopt;
    }
    if (!start_) start_ = clock_.now_ns();
    return clock_.now_ns() - *start_;
}

std::optional<uint64_t> WifiMac::microseconds() {
    std::optional<uint64_t> elapsed = elapsed_ns();
    if (!elapsed) return std::nullopt;
    return (*elapsed / 1000 + offset_us_) & kMask48;
}

// The same value as microseconds() without noticing power changes, for the
// const next_event_ns().
std::optional<uint64_t> WifiMac::peek_microseconds() const {
    if (powered_ && !powered_()) return std::nullopt;
    const uint64_t elapsed = clock_.now_ns() - start_.value_or(clock_.now_ns());
    return (elapsed / 1000 + offset_us_) & kMask48;
}

uint64_t WifiMac::tsf() { return microseconds().value_or(0) + tsf_offset_; }

uint64_t WifiMac::beacon_period_us() const { return uint64_t{core_.peek(kBeaconCntrl1) & 0xFFFF} * 1024; }

void WifiMac::tick(uint64_t now_ns) {
    elapsed_ns();
    if (soft_reset_pending_) {
        soft_reset_pending_ = false;
        pl_.poke(kMacCntrl2, pl_.peek(kMacCntrl2) & ~1u);
        log().log(Level::Debug, name_ + "_PL soft reset done");
    }
    engine_.tick(now_ns);
    if (air_) air_->tick(now_ns);
    fire_timers();
    detect_tbtt();
    update_lines();
}

void WifiMac::fire_timers() {
    if (!armed_) return;
    std::optional<uint64_t> us = microseconds();
    if (!us) return;
    const uint32_t now32 = static_cast<uint32_t>(*us);
    uint32_t       fired = 0;
    for (uint32_t i = 0; i < kAbsTimers; ++i) {
        if ((armed_ >> i & 1) && static_cast<uint32_t>(now32 - targets_[i]) < 0x80000000u) {
            armed_ &= ~(1u << i);
            fired |= 1u << i;
        }
    }
    fired &= pl_.peek(kTimerUnmask);
    if (fired) {
        events_ |= fired;
        general_ |= kGeneralTimers;
    }
}

void WifiMac::detect_tbtt() {
    const uint64_t period = beacon_period_us();
    if (!beacons_active() || period == 0) return;
    if (!microseconds()) return;
    const uint64_t index = tsf() / period;
    if (!last_beacon_) {
        last_beacon_ = index;
        return;
    }
    if (index == *last_beacon_) return;
    last_beacon_ = index;
    if (state_ == kDoze) state_ = next_state_ = kActive;
    general_ |= kGeneralTbtt;
}

std::optional<uint64_t> WifiMac::next_event_ns() const {
    std::optional<uint64_t> earliest_ns = engine_.next_event_ns();
    auto                    merge       = [&earliest_ns](std::optional<uint64_t> t) {
        if (t && (!earliest_ns || *t < *earliest_ns)) earliest_ns = t;
    };
    if (air_) merge(air_->next_event_ns());

    std::optional<uint64_t> us = peek_microseconds();
    if (!us) return earliest_ns;
    std::optional<uint64_t> wait_us;
    auto                    earliest = [&](uint64_t d) { wait_us = wait_us ? std::min(*wait_us, d) : d; };
    for (uint32_t i = 0; i < kAbsTimers; ++i) {
        if (!(armed_ >> i & 1)) continue;
        const uint32_t d = targets_[i] - static_cast<uint32_t>(*us);
        earliest(d >= 0x80000000u ? 0 : d);
    }
    const uint64_t period = beacon_period_us();
    if (beacons_active() && period != 0) {
        const uint64_t now_tsf = *us + tsf_offset_;
        earliest((now_tsf / period + 1) * period - now_tsf);
    }
    if (wait_us) merge(clock_.now_ns() + *wait_us * 1000);
    return earliest_ns;
}

void WifiMac::raise_txrx(uint32_t bits) {
    txrx_ |= bits;
    update_lines();
}

void WifiMac::update_lines() {
    auto drive = [this](size_t i, const Line &line, bool level) {
        if (levels_[i] == static_cast<int>(level)) return;
        levels_[i] = level;
        if (line) line(level);
    };
    const uint32_t general_status = general_ & pl_.peek(kGeneralEnable) & kPending;
    drive(5, lines_.general, (pl_.peek(kGeneralEnable) & kMaster) && general_status != 0);

    const bool     master = pl_.peek(kTxrxEnable) & kMaster;
    const uint32_t status = txrx_ & pl_.peek(kTxrxEnable) & kPending;
    drive(0, lines_.prot_trigger, master && (status & kProtMask));
    drive(1, lines_.tx_trigger, master && (status & kTxMask));
    drive(2, lines_.rx_trigger, master && (status & kRxMask));
    drive(3, lines_.tx_rx_timer, master && (status & kTimerMask));
    drive(4, lines_.tx_rx_misc, master && (status & kMiscMask));
}

uint32_t WifiMac::counter(uint32_t word) {
    std::optional<uint64_t> elapsed = elapsed_ns();
    if (!elapsed) return 0;
    if (word == kCounter1) return static_cast<uint32_t>(*elapsed * 40 / 1000);
    if (word == kTsfLo || word == kTsfHi) {
        const uint64_t t = tsf();
        return word == kTsfLo ? static_cast<uint32_t>(t) : static_cast<uint32_t>(t >> 32);
    }
    const uint64_t us = (*elapsed / 1000 + offset_us_) & kMask48;
    return word == kCounter2Lo ? static_cast<uint32_t>(us) : static_cast<uint32_t>(us >> 32);
}

void WifiMac::software_update(uint32_t value) {
    std::optional<uint64_t> elapsed = elapsed_ns();
    if (elapsed && (value & kSwUpdate)) {
        const uint64_t target = (uint64_t{value & 0xFFFF} << 32) | lo_written_;
        offset_us_            = (target - *elapsed / 1000) & kMask48;
    }
}

void WifiMac::write_tsf(bool high, uint32_t value) {
    std::optional<uint64_t> us = microseconds();
    if (!us) return;
    const uint64_t current = *us + tsf_offset_;
    const uint64_t target =
        high ? (uint64_t{value} << 32) | (current & 0xFFFFFFFF) : (current & ~uint64_t{0xFFFFFFFF}) | value;
    tsf_offset_ = target - *us;
}

void WifiMac::arm_timer(uint32_t index, uint32_t target) {
    targets_[index]            = target;
    std::optional<uint64_t> us = microseconds();
    // A target at or behind the counter never matches: the firmware forces
    // such timers through the event register.
    const uint32_t now32 = us ? static_cast<uint32_t>(*us) : 0;
    if (us && target != now32 && static_cast<uint32_t>(target - now32) < 0x80000000u)
        armed_ |= 1u << index;
    else
        armed_ &= ~(1u << index);
}

uint32_t WifiMac::key_command(uint32_t value) {
    const uint32_t index   = value >> 16 & 0x3FF;
    const uint64_t address = core_.peek(kKeyAddrLo) | uint64_t{core_.peek(kKeyAddrHi) & 0xFFFF} << 32;
    auto           find    = [&](auto pred) { return std::find_if(keys_.begin(), keys_.end(), pred); };

    if (value & kCmdWrite) {
        KeyEntry entry{index, address, {}, value & 0x7FF};
        for (uint32_t i = 0; i < 4; ++i) entry.words[i] = core_.peek(kKeyWord0 + 4 * i);
        auto it = find([&](const KeyEntry &k) { return k.index == index; });
        if (it != keys_.end())
            *it = entry;
        else
            keys_.push_back(entry);
    } else if (value & kCmdRead) {
        auto it = find([&](const KeyEntry &k) { return k.index == index; });
        if (it != keys_.end()) {
            core_.poke(kKeyAddrLo, static_cast<uint32_t>(it->address));
            core_.poke(kKeyAddrHi, static_cast<uint32_t>(it->address >> 32) & 0xFFFF);
            for (uint32_t i = 0; i < 4; ++i) core_.poke(kKeyWord0 + 4 * i, it->words[i]);
            value = (value & ~0x7FFu) | it->attributes;
        }
    } else if (value & kCmdSearch) {
        auto it = find([&](const KeyEntry &k) { return k.address == address; });
        if (it != keys_.end())
            value = (value & ~(kNotFound | 0x3FFu << 16)) | it->index << 16;
        else
            value |= kNotFound;
    }
    return value & ~kCmdBits;
}

std::optional<uint32_t> WifiMac::station_key_index(std::span<const uint8_t> frame) const {
    if (frame.size() < 16) return std::nullopt;
    const uint32_t range = core_.peek(kKeyRange);
    uint32_t       start = range & 0xFF, end = range >> 8 & 0xFF;
    if (range == 0 || end < start) start = 24, end = 63;
    const uint64_t transmitter = mac_address(frame.subspan(10, 6));
    for (const KeyEntry &k : keys_)
        if (k.index >= start && k.index <= end && k.address == transmitter) return k.index;
    return std::nullopt;
}

uint32_t WifiMac::time_on_air(uint32_t value) {
    if (!(value & kSwUpdate)) return value;
    static constexpr double kHtRates[]     = {6.5, 13, 19.5, 26, 39, 52, 58.5, 65};
    static constexpr double kLegacyRates[] = {1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54};
    const uint32_t          p1             = pl_.peek(kAirParam1);
    const uint32_t          length         = p1 & 0xFFFFF;
    const uint32_t          format         = p1 >> 24 & 7;
    const uint32_t          mcs            = pl_.peek(kAirParam2) & 0x7F;
    double                  rate, overhead;
    if (format >= 2) {
        rate     = kHtRates[std::min(mcs, 7u)];
        overhead = 40;
    } else {
        rate     = kLegacyRates[std::min(mcs, 11u)];
        overhead = rate < 6 ? 192 : 20;
    }
    const auto micros = static_cast<uint64_t>(overhead + length * 8 / rate);
    return static_cast<uint32_t>(micros & 0xFFFF) | 1u << 30;
}

void WifiMac::dma_command(uint32_t value) {
    // Queue order matters: the beacon queue goes first.
    static constexpr struct {
        uint32_t queue, head_register, new_head_bit, new_tail_bit;
    } kQueues[] = {
        {4, 0x198, 8,  0},
        {0, 0x19C, 9,  1},
        {1, 0x1A0, 10, 2},
        {2, 0x1A4, 11, 3},
        {3, 0x1A8, 12, 4},
    };

    if (value & (kUseRxHeaderHead | kUseRxPayloadHead)) {
        std::optional<uint32_t> header, payload;
        if (value & kUseRxHeaderHead) header = pl_.peek(kRxHeaderHead);
        if (value & kUseRxPayloadHead) payload = pl_.peek(kRxPayloadHead);
        engine_.set_rx_heads(header, payload);
    }
    for (const auto &q : kQueues) {
        if (value >> q.new_head_bit & 1)
            engine_.transmit(q.queue, pl_.peek(q.head_register), clock_.now_ns());
        else if (value >> q.new_tail_bit & 1)
            engine_.transmit(q.queue, 0, clock_.now_ns());
    }
}

uint32_t WifiMac::core_write_rule(uint32_t word, uint32_t value) {
    switch (word) {
        case kVersion: return 0x00001C03;
        case kStateCntrl: {
            const uint32_t old = state_;
            state_ = next_state_ = value >> 4 & 0xF;
            if (state_ == kIdle && old != kIdle) general_ |= kGeneralIdle;
            if (state_ == kActive && old != kDoze) last_beacon_.reset();
            return state_ << 4 | state_;
        }
        case kErrorRecovery: return value & ~0xFCu;
        case kEncrCntrl: return key_command(value);
        default:
            if (word >= kAbsTimer0 && word < kAbsTimer0 + 4 * kAbsTimers) arm_timer((word - kAbsTimer0) / 4, value);
            return value;
    }
}

uint32_t WifiMac::pl_write_rule(uint32_t word, uint32_t value) {
    switch (word) {
        case kDozeCntrl2:
            if ((value & 1) && state_ == kDoze) state_ = next_state_ = kActive;
            return value;
        case kEventSet:
            events_ |= value & 0x3FF;
            if (events_ & pl_.peek(kTimerUnmask)) general_ |= kGeneralTimers;
            return events_;
        case kEventClear: events_ &= ~value; return events_;
        case kMacCntrl2: soft_reset_pending_ = value & 1; return value;
        case kGeneralAck: general_ &= ~value; return value;
        case kTxrxAck: txrx_ &= ~value; return value;
        case kAirValue: return time_on_air(value);
        case kDmaSet: dma_command(value); return 0;
        case kDmaClear:
        case kDmaStatus1: return 0; // every DMA state machine is idle
        default: return value;
    }
}

void WifiMac::refresh_core(uint32_t word) {
    if (word == kStateCntrl) core_.poke(word, next_state_ << 4 | state_);
}

void WifiMac::refresh_pl(uint32_t word) {
    if (word == kEventSet || word == kEventClear)
        pl_.poke(word, events_);
    else if (word == kGeneralStatus)
        pl_.poke(word, general_ & pl_.peek(kGeneralEnable) & kPending);
    else if (word == kTxrxStatus)
        pl_.poke(word, txrx_ & pl_.peek(kTxrxEnable) & kPending);
}

uint32_t WifiMac::read(const std::string &range, uint32_t offset, uint32_t size) {
    const uint32_t word = offset & ~3u;
    if (range == "pl") {
        if (is_tsf(word)) return counter(word);
        refresh_pl(word);
        return pl_.read("regs", offset, size);
    }
    if (is_counter(word)) return counter(word); // the bus passes whole-word reads only
    refresh_core(word);
    return core_.read("regs", offset, size);
}

void WifiMac::write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) {
    const uint32_t word = offset & ~3u;
    if (size < 4) value &= (1u << (size * 8)) - 1;
    if (range == "pl" && is_tsf(word)) {
        write_tsf(word == kTsfHi, value);
        return;
    }
    if (range == "pl") {
        pl_.write("regs", offset, size, value);
        // The stand-in merged the written bytes; the rule sees the whole word.
        pl_.poke(word, pl_write_rule(word, pl_.peek(word)));
        update_lines();
        return;
    }
    if (!is_counter(word)) {
        core_.write("regs", offset, size, value);
        core_.poke(word, core_write_rule(word, core_.peek(word)));
        update_lines();
        return;
    }
    if (word == kCounter2Lo)
        lo_written_ = value;
    else if (word == kCounter2Hi)
        software_update(value);
    // counter 1 is read-only
}

} // namespace bk
