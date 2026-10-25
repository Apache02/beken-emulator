#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace bk {

// A MAC address for the log: "02:00:00:00:00:02".
inline std::string mac_text(const std::array<uint8_t, 6> &m) {
    return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", m[0], m[1], m[2], m[3], m[4], m[5]);
}

} // namespace bk
