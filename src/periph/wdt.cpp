#include "periph/wdt.h"

#include <utility>

#include "core/log.h"

namespace bk {

namespace {
constexpr uint32_t kDefault = 0x4000;
constexpr uint32_t kMask    = 0xFFFFFF;
constexpr uint32_t kKey1    = 0x5A;
constexpr uint32_t kKey2    = 0xA5;
} // namespace

Wdt::Wdt(std::string name, const Clock &clock, std::function<void()> reset_chip, Signal clock_off)
    : name_(std::move(name)), clock_(clock), reset_chip_(std::move(reset_chip)), clock_off_(std::move(clock_off)),
      reg_(kDefault) {}

uint32_t Wdt::read(const std::string &, uint32_t offset, uint32_t) { return offset == 0 ? reg_ : 0; }

void Wdt::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    if (offset != 0) return;
    reg_          = value & kMask;
    uint32_t key  = (value >> 16) & 0xFF;
    bool     kick = key == kKey2 && key1_seen_;
    key1_seen_    = key == kKey1;
    if (!kick) return;
    if (!running()) {
        start_.reset();
        return;
    }
    uint64_t now = clock_.now_ns();
    Logger::get("wdt").log(Level::Debug,
                           "kick: period " + std::to_string(reg_ & 0xFFFF) + " ms, " +
                               (last_kick_ ? std::to_string((now - *last_kick_) / 1'000'000) + " ms since previous"
                                           : std::string("first kick")));
    last_kick_ = now;
    start_     = now;
}

void Wdt::tick(uint64_t now_ns) {
    if (!running()) {
        start_.reset();
        return;
    }
    if (!start_) start_ = now_ns;
    if (now_ns >= *start_ + period_ns()) {
        Logger::get("wdt").log(Level::Warning, "watchdog expired: resetting the chip");
        start_.reset();
        if (reset_chip_) reset_chip_();
    }
}

std::optional<uint64_t> Wdt::next_event_ns() const {
    if (!start_) return std::nullopt;
    return *start_ + period_ns();
}

void Wdt::reset() {
    reg_       = kDefault;
    key1_seen_ = false;
    start_.reset();
    last_kick_.reset();
}

} // namespace bk
