#include "periph/timer.h"

#include <algorithm>
#include <utility>

namespace bk {

namespace {
constexpr uint32_t kControl = 0x0C;
// Periods are clamped so that time arithmetic stays far from 64-bit wrap.
constexpr uint64_t kMaxPeriod = 1ULL << 62;
constexpr uint64_t kNever     = ~0ULL;

uint64_t saturating_add(uint64_t a, unsigned __int128 b) {
    unsigned __int128 s = static_cast<unsigned __int128>(a) + b;
    return s > kNever ? kNever : static_cast<uint64_t>(s);
}
} // namespace

Timer::Timer(std::string name, const Clock &clock, Line irq, uint32_t hz)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), hz_(hz ? hz : 1) {}

uint64_t Timer::period_ns(uint32_t counter) const {
    unsigned __int128 p =
        static_cast<unsigned __int128>(std::max<uint32_t>(counter, 1)) * (divider_ + 1) * 1'000'000'000ULL / hz_;
    if (p < 1) p = 1;
    return p > kMaxPeriod ? kMaxPeriod : static_cast<uint64_t>(p);
}

void Timer::update_line() {
    if (irq_) irq_(status_ != 0);
}

uint32_t Timer::read(const std::string &, uint32_t offset, uint32_t) {
    if (offset == 0x00 || offset == 0x04 || offset == 0x08) return counter_[offset / 4];
    if (offset == kControl) return enable_ | divider_ << 3 | status_ << 7;
    return 0;
}

void Timer::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    if (offset == 0x00 || offset == 0x04 || offset == 0x08) {
        // Only the reload value changes; schedules (next_) are set by control
        // writes alone, and tick() fires whatever is scheduled.
        counter_[offset / 4] = value;
        return;
    }
    if (offset != kControl) return;
    divider_ = (value >> 3) & 0xF;
    status_ &= ~((value >> 7) & 7);
    enable_      = value & 7;
    uint64_t now = clock_.now_ns();
    for (size_t i = 0; i < 3; ++i) {
        if (!running(i)) {
            next_[i].reset();
        } else if (!next_[i]) {
            next_[i] = saturating_add(now, period_ns(counter_[i]));
        }
    }
    update_line();
}

void Timer::tick(uint64_t now_ns) {
    for (size_t i = 0; i < 3; ++i) {
        if (!next_[i] || *next_[i] > now_ns) continue;
        uint64_t due    = *next_[i];
        uint64_t period = period_ns(counter_[i]);
        status_ |= 1u << i;
        next_[i] = saturating_add(due, static_cast<unsigned __int128>((now_ns - due) / period + 1) * period);
    }
    update_line();
}

std::optional<uint64_t> Timer::next_event_ns() const {
    std::optional<uint64_t> best;
    for (size_t i = 0; i < 3; ++i) {
        if (next_[i] && (!best || *next_[i] < *best)) best = next_[i];
    }
    return best;
}

void Timer::reset() {
    counter_ = {};
    next_    = {};
    enable_ = divider_ = status_ = 0;
    update_line();
}

} // namespace bk
