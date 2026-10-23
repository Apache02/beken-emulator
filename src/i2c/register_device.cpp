#include "i2c/register_device.h"

#include <utility>

namespace bk {

RegisterDevice::RegisterDevice(uint8_t address, std::map<uint8_t, uint8_t> read_only)
    : I2cDevice(address), read_only_(std::move(read_only)) {
    for (const auto &[offset, value] : read_only_) memory_[offset] = value;
}

bool RegisterDevice::begin(bool read) {
    expect_offset_ = !read;
    return true;
}

bool RegisterDevice::write(uint8_t byte) {
    if (expect_offset_) {
        offset_        = byte;
        expect_offset_ = false;
    } else {
        if (!read_only_.count(offset_)) memory_[offset_] = byte;
        offset_ = static_cast<uint8_t>(offset_ + 1);
    }
    return true;
}

uint8_t RegisterDevice::read() {
    uint8_t value = memory_[offset_];
    offset_       = static_cast<uint8_t>(offset_ + 1);
    return value;
}

} // namespace bk
