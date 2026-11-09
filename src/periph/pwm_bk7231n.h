#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// The six-channel PWM of the BK7231N, BK7238 and BK7252N (vendor SDK
// pwm_bk7231n.h): three groups of 0x40 bytes with two channels each. The
// control register of a group holds eight bits per channel (3-bit mode, enable,
// interrupt enable, stop, initial level, update), the pre-divider and the group
// mode; its bits 30 and 31 are the interrupt status of the two channels, write
// 1 to clear. Every channel has four time registers, T1 to T4. In PWM and timer
// mode an enabled channel with a non-zero T4 ends a period every T4 clocks;
// with its interrupt enabled that sets its status bit, and the line is raised
// while any is set. There are no pins: the capture modes never end a period, a
// read request of the CPU register completes at once and the read data
// registers read 0. The clock is `hz` for every channel; the pre-divider, which
// the vendor SDK leaves at 0, is stored but not applied.
class PwmBk7231n : public Module {
  public:
    static constexpr size_t kGroups = 3, kChannelsInGroup = 2, kChannels = kGroups * kChannelsInGroup;

    PwmBk7231n(std::string name, const Clock &clock, Line irq, uint32_t hz = 26000000);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0xC0}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

  private:
    uint32_t &time_reg(uint32_t group, uint32_t reg);
    uint32_t  channel_bits(size_t i) const;
    uint32_t  period_clocks(size_t i) const { return times_[i][3]; }
    uint64_t  period_ns(size_t i) const;
    bool      running(size_t i) const;
    bool      interrupts(size_t i) const;
    void      schedule();
    void      update_line();

    std::string                                    name_;
    const Clock                                   &clock_;
    Line                                           irq_;
    uint32_t                                       hz_;
    std::array<uint32_t, kGroups>                  control_{};
    std::array<std::array<uint32_t, 4>, kChannels> times_{};
    std::array<std::optional<uint64_t>, kChannels> next_{};
    uint32_t                                       status_ = 0; // one bit per channel
};

} // namespace bk
