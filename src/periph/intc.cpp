#include "periph/intc.h"

#include <utility>

namespace bk {

namespace {
constexpr uint32_t kEnable = 0x40, kGlobal = 0x44, kRaw = 0x48, kStatus = 0x4C;
}

Intc::Intc(std::string                       name,
           Line                              cpu_irq,
           Line                              cpu_fiq,
           std::map<uint32_t, uint32_t>      reset_words,
           std::map<std::string, SignalSpec> signals)
    : name_(std::move(name)), cpu_irq_(std::move(cpu_irq)), cpu_fiq_(std::move(cpu_fiq)),
      reset_words_(std::move(reset_words)), words_(reset_words_), signals_(std::move(signals)) {}

Line Intc::line(unsigned bit) {
    size_t index = lines_.size();
    lines_.push_back({bit, false});
    return [this, index](bool level) { set_line(index, level); };
}

uint32_t Intc::raw() const {
    uint32_t r = latched_;
    for (const auto &l : lines_) {
        if (l.level) r |= 1u << (l.bit & 31);
    }
    return r;
}

uint32_t Intc::word(uint32_t offset) const {
    auto it = words_.find(offset);
    return it == words_.end() ? 0 : it->second;
}

void Intc::set_line(size_t index, bool level) {
    lines_[index].level = level;
    if (level) latched_ |= 1u << (lines_[index].bit & 31);
    recompute();
}

void Intc::recompute() {
    uint32_t active = raw() & enable_;
    bool     irq    = (global_ & 1) && (active & 0x0000FFFFu) != 0;
    bool     fiq    = (global_ & 2) && (active & 0xFFFF0000u) != 0;
    if (cpu_irq_) cpu_irq_(irq);
    if (cpu_fiq_) cpu_fiq_(fiq);
}

uint32_t Intc::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kEnable: return enable_;
        case kGlobal: return global_;
        case kRaw: return raw();
        case kStatus: return raw() & enable_;
        default: return word(offset);
    }
}

void Intc::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kEnable: enable_ = value; break;
        case kGlobal: global_ = value & 3; break;
        case kRaw:
        case kStatus: latched_ &= ~value; break;
        default: words_[offset] = value; break;
    }
    recompute();
}

void Intc::reset() {
    latched_ = 0;
    enable_  = 0;
    global_  = 0;
    words_   = reset_words_;
    recompute();
}

Signal Intc::signal(const std::string &signal_name) {
    auto it = signals_.find(signal_name);
    if (it == signals_.end()) return Module::signal(signal_name);
    SignalSpec spec = it->second;
    return [this, spec] {
        uint64_t mask    = spec.width >= 32 ? 0xFFFFFFFFull : ((1ull << spec.width) - 1);
        uint32_t reg     = read("regs", spec.offset, 4);
        uint64_t shifted = spec.shift >= 32 ? 0 : (reg >> spec.shift);
        return (shifted & mask) == spec.equals;
    };
}

} // namespace bk
