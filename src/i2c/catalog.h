#pragma once

#include <memory>
#include <string>

#include "i2c/device.h"

namespace bk {

// `TYPE` or `TYPE@ADDRESS` to a device. The type is case-insensitive; the
// address is a 7-bit number in 0x08-0x77, decimal or with a 0x prefix, and
// defaults to the type's own.
//
// Throws std::invalid_argument: `unknown I2C device 'x' (known: a, b)` or `bad
// I2C device 'x' (want TYPE[@ADDRESS], ADDRESS a 7-bit number 0x08-0x77)`.
std::unique_ptr<I2cDevice> parse_device_spec(const std::string &text);

} // namespace bk
