#pragma once

#include <cstdint>

#include "periph/unknown.h"

namespace bk {

// The SD host with no card in the slot. Registers keep what is written, as in
// Unknown (and are logged the same way), apart from two:
//  - a write to CMD_SEND_CTRL (+0x00) with START (bit 0) set raises TIMEOUT
//    (bit 2) in the status register (+0x24), as a command to an empty slot
//    does, so the driver reports that there is no card;
//  - a write to the status register clears the bits that are set in the
//    value written.
class SdHost : public Unknown {
  public:
    static constexpr uint32_t kCmdSendCtrl = 0x00;
    static constexpr uint32_t kStatus      = 0x24;
    static constexpr uint32_t kStart       = 1u << 0;
    static constexpr uint32_t kTimeout     = 1u << 2;

    using Unknown::Unknown;

    void write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
};

} // namespace bk
