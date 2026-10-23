#include "periph/gpio.h"

#include <stdexcept>
#include <utility>

namespace bk {

namespace {

constexpr uint32_t kBank1Base = 0xC0;

constexpr uint32_t kInput = 1u << 0, kOutput = 1u << 1, kOutputDisable = 1u << 3, kPullMode = 1u << 4,
                   kPullEnable = 1u << 5, kFunctionEnable = 1u << 6;
constexpr uint32_t kCfgWritable = 0x7E;

constexpr uint32_t kIntEnable = 0x84, kIntLevel0 = 0x88, kIntLevel1 = 0x8C, kIntStatus = 0x90, kExtraIntCfg = 0x98,
                   kDetect = 0x9C, kIntEnable2 = 0xE0, kIntLevel3 = 0xE4, kIntStatus2 = 0xE8;

constexpr unsigned kLow = 0, kHigh = 1, kRising = 2, kFalling = 3;

uint32_t pin_offset(unsigned pin) { return pin < 32 ? pin * 4 : kBank1Base + (pin - 32) * 4; }

// The pin whose config word is at `offset`, or Gpio::kPins when none is.
unsigned pin_at(uint32_t offset) {
    if (offset < 32 * 4) return offset / 4;
    if (offset >= kBank1Base && offset < kBank1Base + 8 * 4) return 32 + (offset - kBank1Base) / 4;
    return Gpio::kPins;
}

} // namespace

Gpio::Gpio(std::string name, Line irq, uint32_t pin_reset, std::map<uint32_t, uint32_t> reset_words)
    : name_(std::move(name)), irq_(std::move(irq)), pin_reset_(pin_reset), reset_words_(std::move(reset_words)) {
    reset();
}

void Gpio::reset() {
    regs_.clear();
    for (unsigned p = 0; p < kPins; ++p) regs_[pin_offset(p)] = pin_reset_;
    for (const auto &[offset, value] : reset_words_) regs_[offset] = value;
    latched_ = 0;
    for (unsigned p = 0; p < kPins; ++p) pad_[p] = compute_pad(p);
    update_irq();
}

void Gpio::check_pin(unsigned pin) {
    if (pin >= kPins)
        throw std::invalid_argument("GPIO pin " + std::to_string(pin) + " out of range (0-" +
                                    std::to_string(kPins - 1) + ")");
}

uint32_t Gpio::word(uint32_t offset) const {
    auto it = regs_.find(offset);
    return it == regs_.end() ? 0 : it->second;
}

bool Gpio::pad_level(unsigned pin) const {
    check_pin(pin);
    return pad_[pin];
}

void Gpio::set_input(unsigned pin, std::optional<bool> level) {
    check_pin(pin);
    external_[pin] = level;
    refresh(pin);
}

uint32_t Gpio::read(const std::string &, uint32_t offset, uint32_t) {
    unsigned pin = pin_at(offset);
    if (pin < kPins) return (word(offset) & kCfgWritable) | (pad_[pin] ? kInput : 0);
    if (offset == kIntStatus) return static_cast<uint32_t>(pending());
    if (offset == kIntStatus2) return static_cast<uint32_t>(pending() >> 32);
    return word(offset);
}

void Gpio::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    unsigned pin = pin_at(offset);
    if (pin < kPins) {
        regs_[offset] = value & kCfgWritable;
        refresh(pin);
    } else if (offset == kIntStatus) {
        latched_ &= ~static_cast<uint64_t>(value);
        settle();
    } else if (offset == kIntStatus2) {
        latched_ &= ~(static_cast<uint64_t>(value) << 32);
        settle();
    } else if (offset == kDetect) {
        // Read-only.
    } else if (offset == kExtraIntCfg) {
        regs_[offset] = value & 0x33; // the write-1-to-clear status bits have no source
    } else {
        regs_[offset] = value;
        if (offset == kIntEnable || offset == kIntEnable2 || offset == kIntLevel0 || offset == kIntLevel1 ||
            offset == kIntLevel3)
            settle();
    }
}

bool Gpio::compute_pad(unsigned pin) const {
    uint32_t cfg = word(pin_offset(pin));
    if (!(cfg & kOutputDisable) && !(cfg & kFunctionEnable)) return (cfg & kOutput) != 0;
    if (external_[pin]) return *external_[pin];
    if (cfg & kPullEnable) return (cfg & kPullMode) != 0;
    return false;
}

void Gpio::refresh(unsigned pin) {
    bool now = compute_pad(pin);
    bool old = pad_[pin];
    if (now != old) {
        pad_[pin] = now;
        if (enabled(pin)) {
            unsigned m = mode(pin);
            if ((m == kRising && now) || (m == kFalling && !now)) latched_ |= uint64_t{1} << pin;
        }
        for (const auto &watcher : watchers_) watcher(pin, now);
    }
    settle();
}

bool Gpio::enabled(unsigned pin) const {
    uint32_t reg = word(pin < 32 ? kIntEnable : kIntEnable2);
    return (reg >> (pin < 32 ? pin : pin - 32)) & 1;
}

unsigned Gpio::mode(unsigned pin) const {
    uint32_t reg;
    unsigned index;
    if (pin < 16) {
        reg   = word(kIntLevel0);
        index = pin;
    } else if (pin < 32) {
        reg   = word(kIntLevel1);
        index = pin - 16;
    } else {
        reg   = word(kIntLevel3);
        index = pin - 32;
    }
    return (reg >> (index * 2)) & 3;
}

// Level modes latch for as long as their level holds.
void Gpio::settle() {
    for (unsigned p = 0; p < kPins; ++p) {
        if (!enabled(p)) continue;
        unsigned m = mode(p);
        if ((m == kLow && !pad_[p]) || (m == kHigh && pad_[p])) latched_ |= uint64_t{1} << p;
    }
    update_irq();
}

uint64_t Gpio::pending() const {
    uint64_t on = 0;
    for (unsigned p = 0; p < kPins; ++p) {
        if (enabled(p)) on |= uint64_t{1} << p;
    }
    return latched_ & on;
}

void Gpio::update_irq() {
    if (irq_) irq_(pending() != 0);
}

} // namespace bk
