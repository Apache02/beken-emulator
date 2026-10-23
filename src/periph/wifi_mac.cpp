#include "periph/wifi_mac.h"

#include <utility>

namespace bk {

namespace {

constexpr uint32_t kCounter1 = 0x11C, kCounter2Lo = 0x120, kCounter2Hi = 0x124;
constexpr uint32_t kSwUpdate = 1u << 31;
constexpr uint64_t kMask48   = (uint64_t{1} << 48) - 1;

bool is_counter(uint32_t word) { return word == kCounter1 || word == kCounter2Lo || word == kCounter2Hi; }

} // namespace

WifiMac::WifiMac(std::string name, const Clock &clock, Signal powered, Unknown::InstructionSource instruction)
    : name_(std::move(name)), clock_(clock), powered_(std::move(powered)),
      core_(name_ + "_CORE", kCoreSize, {}, instruction), pl_(name_ + "_PL", kPlSize, {}, instruction) {
    reset();
}

void WifiMac::reset() {
    core_.reset();
    pl_.reset();
    start_      = powered_ ? std::nullopt : std::optional<uint64_t>(0);
    offset_us_  = 0;
    lo_written_ = 0;
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

void WifiMac::tick(uint64_t) { elapsed_ns(); }

uint32_t WifiMac::counter(uint32_t word) {
    std::optional<uint64_t> elapsed = elapsed_ns();
    if (!elapsed) return 0;
    if (word == kCounter1) return static_cast<uint32_t>(*elapsed * 40 / 1000);
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

uint32_t WifiMac::read(const std::string &range, uint32_t offset, uint32_t size) {
    if (range == "pl") return pl_.read("regs", offset, size);
    const uint32_t word = offset & ~3u;
    if (!is_counter(word)) return core_.read("regs", offset, size);
    return counter(word); // the bus passes whole-word reads only
}

void WifiMac::write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) {
    if (range == "pl") {
        pl_.write("regs", offset, size, value);
        return;
    }
    const uint32_t word = offset & ~3u;
    if (!is_counter(word)) {
        core_.write("regs", offset, size, value);
        return;
    }
    if (size < 4) value &= (1u << (size * 8)) - 1;
    if (word == kCounter2Lo)
        lo_written_ = value;
    else if (word == kCounter2Hi)
        software_update(value);
    // counter 1 is read-only
}

} // namespace bk
