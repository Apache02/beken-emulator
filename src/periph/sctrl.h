#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// Biases a register field on read: the field [shift +: width] of the word at
// `offset` reads as its stored value plus `add`, clamped to the field maximum.
struct ReadAdjust {
    uint32_t offset;
    unsigned shift, width;
    uint32_t add;
};

// System control: chip identity plus plain 32-bit words.
class Sctrl : public Module {
  public:
    Sctrl(std::string                       name,
          uint32_t                          chip_id,
          uint32_t                          device_id,
          std::map<uint32_t, uint32_t>      reset_words,
          std::map<uint32_t, uint32_t>      warm_reset_words,
          std::map<std::string, SignalSpec> signals,
          std::vector<ReadAdjust>           read_adjust);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x200}
        };
    }
    uint32_t read(const std::string &range, uint32_t offset, uint32_t size) override;
    void     write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    // Words become the cold values overlaid with the warm ones. The initial
    // state holds the cold values alone.
    void   reset() override;
    Signal signal(const std::string &signal_name) override;

  private:
    uint32_t stored(uint32_t offset) const;

    std::string                       name_;
    uint32_t                          chip_id_, device_id_;
    std::map<uint32_t, uint32_t>      reset_words_, warm_reset_words_, words_;
    std::map<std::string, SignalSpec> signals_;
    std::vector<ReadAdjust>           read_adjust_;
};

} // namespace bk
