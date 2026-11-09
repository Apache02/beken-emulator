#include "periph/pwm_bk7231n.h"

#include <utility>

namespace bk {

namespace {
constexpr uint32_t kGroupSize = 0x40, kControl = 0x00, kFirstTime = 0x04, kChannelTimes = 0x10, kLastTime = 0x20;
constexpr uint32_t kControlMask = 0x03FFFFFF, kStatusShift = 30;
constexpr uint32_t kBitsPerChannel = 8, kModeMask = 7, kEnable = 1u << 3, kInterrupt = 1u << 4;
constexpr uint32_t kPwmMode = 1, kTimerMode = 2;
// Periods are clamped so that time arithmetic stays far from 64-bit wrap.
constexpr uint64_t kMaxPeriod = 1ULL << 62;
constexpr uint64_t kNever     = ~0ULL;

uint64_t saturating_add(uint64_t a, unsigned __int128 b) {
    unsigned __int128 s = static_cast<unsigned __int128>(a) + b;
    return s > kNever ? kNever : static_cast<uint64_t>(s);
}
} // namespace

PwmBk7231n::PwmBk7231n(std::string name, const Clock &clock, Line irq, uint32_t hz)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), hz_(hz ? hz : 1) {}

uint32_t PwmBk7231n::channel_bits(size_t i) const {
    return control_[i / kChannelsInGroup] >> (kBitsPerChannel * (i % kChannelsInGroup)) & 0xFF;
}

uint32_t &PwmBk7231n::time_reg(uint32_t group, uint32_t reg) {
    uint32_t r = reg - kFirstTime;
    return times_[group * kChannelsInGroup + r / kChannelTimes][r % kChannelTimes / 4];
}

uint64_t PwmBk7231n::period_ns(size_t i) const {
    unsigned __int128 p = static_cast<unsigned __int128>(period_clocks(i)) * 1'000'000'000ULL / hz_;
    if (p < 1) p = 1;
    return p > kMaxPeriod ? kMaxPeriod : static_cast<uint64_t>(p);
}

bool PwmBk7231n::running(size_t i) const {
    uint32_t bits = channel_bits(i), mode = bits & kModeMask;
    return (bits & kEnable) && (mode == kPwmMode || mode == kTimerMode) && period_clocks(i) > 0;
}

bool PwmBk7231n::interrupts(size_t i) const { return channel_bits(i) & kInterrupt; }

// A channel that starts running ends its first period a full period from now;
// one that keeps running keeps its schedule.
void PwmBk7231n::schedule() {
    uint64_t now = clock_.now_ns();
    for (size_t i = 0; i < kChannels; ++i) {
        if (!running(i))
            next_[i].reset();
        else if (!next_[i])
            next_[i] = saturating_add(now, period_ns(i));
    }
}

void PwmBk7231n::update_line() {
    if (irq_) irq_(status_ != 0);
}

uint32_t PwmBk7231n::read(const std::string &, uint32_t offset, uint32_t) {
    uint32_t group = offset / kGroupSize, reg = offset % kGroupSize;
    if (group >= kGroups) return 0;
    if (reg == kControl) {
        uint32_t status = status_ >> (group * kChannelsInGroup) & 3;
        return control_[group] | status << kStatusShift;
    }
    if (reg < kFirstTime || reg > kLastTime + 3) return 0; // CPU read request, read data
    return time_reg(group, reg);
}

void PwmBk7231n::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    uint32_t group = offset / kGroupSize, reg = offset % kGroupSize;
    if (group >= kGroups) return;
    if (reg == kControl) {
        control_[group] = value & kControlMask;
        status_ &= ~((value >> kStatusShift) << (group * kChannelsInGroup));
        // A channel whose interrupt is turned off raises it no more.
        for (size_t i = 0; i < kChannels; ++i) {
            if (!interrupts(i)) status_ &= ~(1u << i);
        }
    } else if (reg >= kFirstTime && reg <= kLastTime + 3) {
        time_reg(group, reg) = value;
    }
    schedule();
    update_line();
}

void PwmBk7231n::tick(uint64_t now_ns) {
    for (size_t i = 0; i < kChannels; ++i) {
        if (!next_[i] || *next_[i] > now_ns) continue;
        uint64_t due    = *next_[i];
        uint64_t period = period_ns(i);
        if (interrupts(i)) status_ |= 1u << i;
        next_[i] = saturating_add(due, static_cast<unsigned __int128>((now_ns - due) / period + 1) * period);
    }
    update_line();
}

std::optional<uint64_t> PwmBk7231n::next_event_ns() const {
    std::optional<uint64_t> best;
    for (size_t i = 0; i < kChannels; ++i) {
        if (next_[i] && (!best || *next_[i] < *best)) best = next_[i];
    }
    return best;
}

void PwmBk7231n::reset() {
    control_ = {};
    times_   = {};
    next_    = {};
    status_  = 0;
    update_line();
}

} // namespace bk
