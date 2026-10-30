#include "periph/intc.h"

#include <cstdio>
#include <utility>

#include "core/log.h"
#include "core/signal.h"

namespace bk {

namespace {
constexpr uint32_t kEnable = 0x40, kGlobal = 0x44, kRaw = 0x48, kStatus = 0x4C;

Logger &log() { return Logger::get("intc"); }
bool    debug() { return log().level() <= Level::Debug; }

std::string hex(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", value);
    return buf;
}

// The source numbers of the set bits, for example "0,8,17", or "none".
std::string sources(uint32_t bits) {
    std::string out;
    for (unsigned b = 0; b < 32; ++b)
        if (bits >> b & 1) out += (out.empty() ? "" : ",") + std::to_string(b);
    return out.empty() ? "none" : out;
}
} // namespace

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
    if (lines_[index].level != level && debug()) {
        const unsigned bit = lines_[index].bit & 31;
        log().log(Level::Debug,
                  "source " + std::to_string(bit) + (level ? " high" : " low") +
                      ((enable_ >> bit & 1) ? "" : " (not enabled)"));
    }
    lines_[index].level = level;
    if (level) latched_ |= 1u << (lines_[index].bit & 31);
    recompute();
}

void Intc::recompute() {
    uint32_t active = raw() & enable_;
    bool     irq    = (global_ & 1) && (active & 0x0000FFFFu) != 0;
    bool     fiq    = (global_ & 2) && (active & 0xFFFF0000u) != 0;
    if (irq != irq_out_ && debug())
        log().log(Level::Debug, std::string("CPU IRQ ") + (irq ? "high, sources " + sources(active & 0xFFFFu) : "low"));
    if (fiq != fiq_out_ && debug())
        log().log(Level::Debug,
                  std::string("CPU FIQ ") + (fiq ? "high, sources " + sources(active >> 16 << 16) : "low"));
    irq_out_ = irq;
    fiq_out_ = fiq;
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
        case kEnable:
            if (value != enable_ && debug())
                log().log(Level::Debug,
                          "enable <= " + hex(value) + " (on: " + sources(value & ~enable_) +
                              "; off: " + sources(enable_ & ~value) + ")");
            enable_ = value;
            break;
        case kGlobal:
            if ((value & 3) != global_ && debug())
                log().log(Level::Debug,
                          std::string("global <= IRQ ") + (value & 1 ? "on" : "off") + ", FIQ " +
                              (value & 2 ? "on" : "off"));
            global_ = value & 3;
            break;
        case kRaw:
        case kStatus: {
            const uint32_t cleared = latched_ & value;
            latched_ &= ~value;
            // A source whose line is still high stays raised.
            if (debug())
                log().log(Level::Debug,
                          "clear " + hex(value) + ": cleared " + sources(cleared) + ", still raised " +
                              sources(raw() & value));
            break;
        }
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
    return register_signal([this](uint32_t offset) { return read("regs", offset, 4); }, it->second);
}

} // namespace bk
