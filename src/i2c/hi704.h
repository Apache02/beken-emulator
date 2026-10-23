#pragma once

#include "i2c/register_device.h"

namespace bk {

// HI704 VGA camera sensor: 256 registers, the chip id 0x96 at register 0x04.
class Hi704 : public RegisterDevice {
  public:
    static constexpr uint8_t kDefaultAddress = 0x30;

    explicit Hi704(uint8_t address = kDefaultAddress)
        : RegisterDevice(address,
                         {
                             {0x04, 0x96}
    }) {}

    std::string type() const override { return "hi704"; }
};

} // namespace bk
