#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "i2c/device.h"

namespace bk {

// A device that looks like 256 bytes of memory. The first byte of a write
// transaction sets the offset; every further byte is stored there and the
// offset moves on. A read returns the byte at the offset and moves it on. The
// offset wraps at 256 and survives a STOP, so a write of the offset can be
// followed by a separate read.
//
// `read_only` maps an offset to a fixed value: that register reads the value
// and ignores writes.
class RegisterDevice : public I2cDevice {
  public:
    RegisterDevice(uint8_t address, std::map<uint8_t, uint8_t> read_only = {});

    std::string type() const override { return "register"; }
    bool        begin(bool read) override;
    bool        write(uint8_t byte) override;
    uint8_t     read() override;

  private:
    std::array<uint8_t, 256>   memory_{};
    std::map<uint8_t, uint8_t> read_only_;
    uint8_t                    offset_        = 0;
    bool                       expect_offset_ = false;
};

} // namespace bk
