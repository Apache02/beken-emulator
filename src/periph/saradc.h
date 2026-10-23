#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// SAR ADC without an analog input. While `config.chnl_en` is set a sample
// arrives every 100 us of device time in a 32-sample FIFO; every sample reads
// `sample`. The interrupt is raised once ctrl_cfg.almost_cfg samples wait (at
// least one) and stays until config.int_clr is written.
class Saradc : public Module {
  public:
    // `irq` may be empty.
    Saradc(std::string name, Line irq, uint32_t sample);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x20}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override { return next_ns_; }
    void                    reset() override;

  private:
    void     write_config(uint32_t value);
    uint32_t pop(uint32_t when_empty);
    void     update_irq();

    std::string             name_;
    Line                    irq_;
    uint32_t                sample_;
    uint32_t                config_ = 0, ctrl_cfg_ = 0, saturation_cfg_ = 0;
    uint64_t                last_tick_ = 0;
    uint32_t                fifo_      = 0; // samples waiting
    std::optional<uint64_t> next_ns_;
    bool                    pending_ = false;
};

} // namespace bk
