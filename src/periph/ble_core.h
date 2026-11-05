#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// The BLE 4.2 core (RW-BLE) as far as firmware needs it to start without a
// radio: the register block `regs` and the exchange memory `em` it shares with
// the core.
//
// The registers read back what was written, except:
//  - BASETIMECNT (0x1C): writing SAMP (bit 31) samples the time of the clock
//    into BASETIMECNT [26:0], in 625 us slots, and FINETIMECNT (0x20) [9:0],
//    the microseconds left in the slot (624 down to 0); SAMP reads 0, the
//    sampling being instant. The BLE stack busy-waits with interrupts off for
//    the fine counter to say enough of the slot is left.
//  - RWBLECNTL (0x00): the soft reset and software interrupt request bits
//    [31:28] read 0, their operation completing at once.
// Nothing is sent or received and no interrupt is raised.
//
// The exchange memory is RAM with byte enables (see Module::byte_lanes): the
// stack keeps its control structures and descriptors there in halfwords.
class BleCore : public Module {
  public:
    static constexpr uint32_t kRegsSize = 0x200;

    BleCore(std::string name, const Clock &clock, uint32_t em_size = 0x2000);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", kRegsSize},
            {"em",   em_size_ }
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    bool     byte_lanes(const std::string &range) const override { return range == "em"; }
    void     reset() override;

  private:
    std::string           name_;
    const Clock          &clock_;
    uint32_t              em_size_;
    std::vector<uint32_t> regs_;
    std::vector<uint8_t>  em_;
};

} // namespace bk
