#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// The BK7238 SAR ADC (also in the BK7252N) without an analog input. While
// config0.start is set and state enables one of its interrupts, a sample
// arrives every 100 us of device time in an 8-sample FIFO; every sample reads
// `sample`. Each sample sets state.fifo_int and state.conv_done (write 1 to
// clear); the interrupt is raised while a set flag has its enable bit.
class SaradcBk7238 : public Module {
  public:
    // `irq` may be empty.
    SaradcBk7238(std::string name, Line irq, uint32_t sample);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x1C}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override { return next_ns_; }
    void                    reset() override;

  private:
    bool     running() const;
    void     update_schedule();
    uint32_t state() const;
    void     update_irq();

    std::string             name_;
    Line                    irq_;
    uint32_t                sample_;
    uint32_t                config0_ = 0, config1_ = 0, config2_ = 0, config5_ = 0, config6_ = 0;
    uint32_t                state_     = 0; // the stored bits: enables and the flags
    uint64_t                last_tick_ = 0;
    uint32_t                fifo_      = 0; // samples waiting
    std::optional<uint64_t> next_ns_;
};

} // namespace bk
