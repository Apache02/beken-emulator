#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace bk {

// The four coefficients of the flash cipher, coef0 first. The eFuse holds
// them in bytes 0..15 as little-endian words; tools print them as the word
// values, for example "510fb093 a3cbeadc 5993a17e c7adeb03".
using FlashKey = std::array<uint32_t, 4>;

// The key in eFuse bytes 0..15 of `fuses` (at least 16 bytes), and back.
FlashKey                key_from_fuses(std::span<const uint8_t> fuses);
std::array<uint8_t, 16> fuses_from_key(const FlashKey &key);
// The word values, "510fb093 a3cbeadc 5993a17e c7adeb03".
std::string key_text(const FlashKey &key);

// The cipher the flash controller applies to the data it reads through the
// XIP window: every 32-bit word is XORed with a mask of its address in the
// window (data bytes only, CRC bytes not counted) and the key. The mask XORs
// three address scramblers (pn15, pn16, pn32) and a constant (coef2); coef3
// switches each of them off and picks how the address is fed to them. The
// cipher is off (every mask 0) when coef3[31:24] is 0x00 or 0xFF, which a
// blank eFuse gives, or when coef3[3:0] switches everything off.
class FlashCipher {
  public:
    FlashCipher() = default;
    explicit FlashCipher(const FlashKey &key);

    const FlashKey &key() const { return key_; }
    bool            bypass() const { return bypass_; }
    // The mask of the word at `addr` (a multiple of 4); 0 when bypassed.
    uint32_t mask(uint32_t addr) const;

  private:
    FlashKey key_{};
    bool     bypass_ = true;
    bool     pn15_ = false, pn16_ = false, pn32_ = false;
    unsigned pn15_sel_ = 0, pn16_sel_ = 0, pn32_sel_ = 0;
    uint32_t pn16_mix_ = 0, random_ = 0;
};

// What the start of a flash image tells about its cipher. Every Beken image
// starts with the same vector table: a branch, then seven `ldr pc, [pc, #0x14]`.
struct KeyDetection {
    enum class Kind {
        Blank,   // no block at a 4 KiB boundary of the window has a valid CRC
        Plain,   // a vector table is there as it is
        Known,   // a vector table is there under one of the known keys
        Unknown, // blocks with a valid CRC, but no vector table under any key
    };
    Kind        kind = Kind::Blank;
    FlashKey    key{};    // Known: the key
    std::string name;     // Known: the name of the key
    uint32_t    addr = 0; // Plain, Known: the window address of the vector table; Unknown: of the first valid block
};

// The window address of the first vector table under `cipher` in the physical
// flash contents `raw` (34-byte blocks: 32 data bytes and their CRC). Only the
// first block of every 4 KiB of the window counts, and only with a valid CRC.
std::optional<uint32_t> find_vector_table(std::span<const uint8_t> raw, const FlashCipher &cipher);

// Looks for a vector table in `raw` as find_vector_table does: plain first,
// then under each known key.
KeyDetection detect_key(std::span<const uint8_t> raw);

} // namespace bk
