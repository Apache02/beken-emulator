#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// The six-channel PWM of the BK7221U and BK7231U (vendor SDK pwm.h, at
// PWM_NEW_BASE + 0x80). Control holds four bits per channel (enable, interrupt
// enable, a 2-bit mode), the interrupt status is write 1 to clear, then every
// channel has an end value, a duty cycle and a capture register. In PWM and
// timer mode an enabled channel with a non-zero end value ends a period every
// `end` clocks; with its interrupt enabled that sets its status bit, and the
// line is raised while any is set. There are no pins: the capture modes never
// end a period and the capture registers read 0. The clock is `hz` for every
// channel (the ICU PWM clock mux, which can pick the 32 kHz LPO, is not
// modelled).
class Pwm : public Module {
  public:
    static constexpr size_t kChannels = 6;

    Pwm(std::string name, const Clock &clock, Line irq, uint32_t hz = 26000000);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x50}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

  private:
    uint64_t period_ns(uint32_t end) const;
    bool     running(size_t i) const;
    bool     interrupts(size_t i) const { return control_ >> (4 * i + 1) & 1; }
    void     schedule();
    void     update_line();

    std::string                                    name_;
    const Clock                                   &clock_;
    Line                                           irq_;
    uint32_t                                       hz_;
    std::array<uint32_t, kChannels>                end_{}, duty_{};
    std::array<std::optional<uint64_t>, kChannels> next_{};
    uint32_t                                       control_ = 0, status_ = 0;
};

} // namespace bk
