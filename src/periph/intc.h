#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// Interrupt controller: source bits 0-15 request IRQ, 16-31 request FIQ.
class Intc : public Module {
  public:
    Intc(std::string                       name,
         Line                              cpu_irq,
         Line                              cpu_fiq,
         std::map<uint32_t, uint32_t>      reset_words,
         std::map<std::string, SignalSpec> signals);

    // The returned Line stays valid for the lifetime of this Intc.
    Line line(unsigned bit) override;

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x54}
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void     reset() override;
    Signal   signal(const std::string &signal_name) override;

  private:
    struct LineState {
        unsigned bit;
        bool     level;
    };

    uint32_t raw() const;
    uint32_t word(uint32_t offset) const;
    void     set_line(size_t index, bool level);
    void     recompute();

    std::string                       name_;
    Line                              cpu_irq_, cpu_fiq_;
    std::map<uint32_t, uint32_t>      reset_words_, words_;
    std::map<std::string, SignalSpec> signals_;
    std::vector<LineState>            lines_;
    uint32_t                          latched_ = 0, enable_ = 0, global_ = 0;
};

} // namespace bk
