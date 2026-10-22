#pragma once

#include <cstdint>
#include <span>

namespace bk {

// CRC-16 protecting each 32-byte flash block: polynomial 0x8005, initial value
// 0xFFFF, MSB first, no reflection, no final XOR.
inline uint16_t crc16(std::span<const uint8_t> data32) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data32) {
        crc ^= static_cast<uint16_t>(byte << 8);
        for (int bit = 0; bit < 8; ++bit) crc = static_cast<uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

} // namespace bk
