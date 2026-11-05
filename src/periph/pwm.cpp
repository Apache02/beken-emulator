#include "periph/pwm.h"

#include <utility>

namespace bk {

namespace {
constexpr uint32_t kControl = 0x00, kStatus = 0x04, kFirstChannel = 0x08, kChannelSize = 0x0C;
constexpr uint32_t kPwmMode = 0, kTimerMode = 1;
// Periods are clamped so that time arithmetic stays far from 64-bit wrap.
constexpr uint64_t kMaxPeriod = 1ULL << 62;
constexpr uint64_t kNever     = ~0ULL;

uint64_t saturating_add(uint64_t a, unsigned __int128 b) {
    unsigned __int128 s = static_cast<unsigned __int128>(a) + b;
    return s > kNever ? kNever : static_cast<uint64_t>(s);
}
} // namespace

Pwm::Pwm(std::string name, const Clock &clock, Line irq, uint32_t hz)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), hz_(hz ? hz : 1) {}

uint64_t Pwm::period_ns(uint32_t end) const {
    unsigned __int128 p = static_cast<unsigned __int128>(end) * 1'000'000'000ULL / hz_;
    if (p < 1) p = 1;
    return p > kMaxPeriod ? kMaxPeriod : static_cast<uint64_t>(p);
}

bool Pwm::running(size_t i) const {
    uint32_t bits = control_ >> (4 * i);
    uint32_t mode = bits >> 2 & 3;
    return (bits & 1) && (mode == kPwmMode || mode == kTimerMode) && end_[i] > 0;
}

// A channel that starts running ends its first period a full period from now;
// one that keeps running keeps its schedule.
void Pwm::schedule() {
    uint64_t now = clock_.now_ns();
    for (size_t i = 0; i < kChannels; ++i) {
        if (!running(i))
            next_[i].reset();
        else if (!next_[i])
            next_[i] = saturating_add(now, period_ns(end_[i]));
    }
}

void Pwm::update_line() {
    if (irq_) irq_(status_ != 0);
}

uint32_t Pwm::read(const std::string &, uint32_t offset, uint32_t) {
    if (offset == kControl) return control_;
    if (offset == kStatus) return status_;
    if (offset < kFirstChannel) return 0;
    uint32_t i = (offset - kFirstChannel) / kChannelSize, reg = (offset - kFirstChannel) % kChannelSize;
    if (i >= kChannels) return 0;
    if (reg == 0) return end_[i];
    if (reg == 4) return duty_[i];
    return 0; // capture
}

void Pwm::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    if (offset == kControl) {
        control_ = value & 0x00FFFFFF;
        // A channel whose interrupt is turned off raises it no more.
        for (size_t i = 0; i < kChannels; ++i) {
            if (!interrupts(i)) status_ &= ~(1u << i);
        }
    } else if (offset == kStatus) {
        status_ &= ~(value & 0x3F);
    } else if (offset >= kFirstChannel) {
        uint32_t i = (offset - kFirstChannel) / kChannelSize, reg = (offset - kFirstChannel) % kChannelSize;
        if (i >= kChannels) return;
        if (reg == 0) end_[i] = value;
        if (reg == 4) duty_[i] = value;
    }
    schedule();
    update_line();
}

void Pwm::tick(uint64_t now_ns) {
    for (size_t i = 0; i < kChannels; ++i) {
        if (!next_[i] || *next_[i] > now_ns) continue;
        uint64_t due    = *next_[i];
        uint64_t period = period_ns(end_[i]);
        if (interrupts(i)) status_ |= 1u << i;
        next_[i] = saturating_add(due, static_cast<unsigned __int128>((now_ns - due) / period + 1) * period);
    }
    update_line();
}

std::optional<uint64_t> Pwm::next_event_ns() const {
    std::optional<uint64_t> best;
    for (size_t i = 0; i < kChannels; ++i) {
        if (next_[i] && (!best || *next_[i] < *best)) best = next_[i];
    }
    return best;
}

void Pwm::reset() {
    end_     = {};
    duty_    = {};
    next_    = {};
    control_ = status_ = 0;
    update_line();
}

} // namespace bk
