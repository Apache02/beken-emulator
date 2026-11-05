#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "periph/crc16.h"
#include "periph/flash_cipher.h"

using namespace bk;

namespace {

constexpr FlashKey kTuya{0x510FB093, 0xA3CBEADC, 0x5993A17E, 0xC7ADEB03};

// The physical flash: `blocks` erased blocks.
std::vector<uint8_t> erased(uint32_t blocks) { return std::vector<uint8_t>(blocks * 34, 0xFF); }

// Writes the 32 data bytes of block `n`, XORed with the masks of `cipher`, and their CRC.
void put_block(std::vector<uint8_t> &raw, uint32_t n, const std::array<uint32_t, 8> &words, const FlashCipher &cipher) {
    uint8_t *b = raw.data() + n * 34;
    for (uint32_t w = 0; w < 8; ++w) {
        uint32_t v = words[w] ^ cipher.mask(n * 32 + 4 * w);
        for (uint32_t i = 0; i < 4; ++i) b[4 * w + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    uint16_t crc = crc16(std::span<const uint8_t>(b, 32));
    b[32]        = static_cast<uint8_t>(crc >> 8);
    b[33]        = static_cast<uint8_t>(crc);
}

constexpr std::array<uint32_t, 8> kVectors{
    0xEA00004B, 0xE59FF014, 0xE59FF014, 0xE59FF014, 0xE59FF014, 0xE59FF014, 0xE59FF014, 0xE59FF014};
constexpr std::array<uint32_t, 8> kOther{1, 2, 3, 4, 5, 6, 7, 8};

} // namespace

TEST_CASE("flash cipher masks match the hardware for every scrambler setting") {
    const uint32_t addrs[] = {0, 4, 0x1C, 0x1000, 0x12344, 0x1FFFFC, 0xABCDE0};
    struct Case {
        FlashKey                key;
        std::array<uint32_t, 7> masks;
    };
    const Case cases[] = {
        // pn32 and the constant, the address rotated by 8
        {kTuya,                                            {0x7CB50721, 0x7CB50F21, 0x7CB53F21, 0x38950361, 0xFEFB0FA1, 0xA6535BD0, 0x0A3FE711}},
        // everything on, every select 3, the pn16 bit set
        {{0x01234567, 0x89ABCDEF, 0x0BADF00D, 0x5A001B70},
         {0x1638A374, 0x1E30A374, 0x2E00A2FC, 0x36388154, 0xD9A3816A, 0x728E552D, 0x2ECE3EE0}                                                  },
        // everything on, selects 1, 2, 1
        {{0xDEADBEEF, 0x13572468, 0xCAFEBABE, 0x3C000A20},
         {0x9A3E9706, 0x9A369F86, 0x9A06AC8E, 0xFE1E9347, 0x5DE9F714, 0xE2500251, 0x0245FD5B}                                                  },
        // everything on, selects 0, 0, 1
        {{0x11223344, 0x55667788, 0x99AABBCC, 0x81000800},
         {0x11A8EFB3, 0x19A8E5B3, 0x29A8D9B3, 0x55A8EBF7, 0x3B80776A, 0x6FC07B6C, 0xD298FBB0}                                                  },
        // pn32 off, selects 2, 1, the pn16 bit set
        {{0x0F0F0F0F, 0xF00FF00F, 0x12345678, 0x42000154},
         {0x0DD4C199, 0x05D4C091, 0x35D4C691, 0x0DF4C19B, 0xA7B014F5, 0x976408BF, 0xAE688FB1}                                                  },
        // the constant off, selects 2, 0, 2
        {{0x76543210, 0xFEDCBA98, 0x55AA55AA, 0x42001048},
         {0xFBBDB4A4, 0xF3BDB6AC, 0xC3BDBA9C, 0xFB9D94A0, 0x51DB6275, 0x0423A703, 0xF95E7BF7}                                                  },
    };
    for (const Case &c : cases) {
        FlashCipher cipher(c.key);
        CHECK_FALSE(cipher.bypass());
        for (size_t i = 0; i < std::size(addrs); ++i) {
            CAPTURE(c.key[3]);
            CAPTURE(addrs[i]);
            CHECK(cipher.mask(addrs[i]) == c.masks[i]);
        }
    }
}

TEST_CASE("flash cipher is off for a blank key, coef3[31:24] 0xFF and coef3[3:0] 0xF") {
    for (const FlashKey &key : {
             FlashKey{},
             FlashKey{1, 2, 3, 0xFF000000},
             FlashKey{1, 2, 3, 0x4200000F}
    }) {
        FlashCipher cipher(key);
        CHECK(cipher.bypass());
        CHECK(cipher.mask(0x1234) == 0);
    }
    CHECK(FlashCipher().bypass());
}

TEST_CASE("flash key is little-endian words in eFuse bytes 0..15") {
    const std::array<uint8_t, 16> fuses{
        0x93, 0xB0, 0x0F, 0x51, 0xDC, 0xEA, 0xCB, 0xA3, 0x7E, 0xA1, 0x93, 0x59, 0x03, 0xEB, 0xAD, 0xC7};
    CHECK(key_from_fuses(fuses) == kTuya);
    CHECK(fuses_from_key(kTuya) == fuses);
    CHECK(key_text(kTuya) == "510fb093 a3cbeadc 5993a17e c7adeb03");
}

TEST_CASE("key detection finds a plain image") {
    auto raw = erased(512);
    put_block(raw, 0, kVectors, FlashCipher());
    KeyDetection d = detect_key(raw);
    CHECK(d.kind == KeyDetection::Kind::Plain);
    CHECK(d.addr == 0);
}

TEST_CASE("key detection finds the Tuya key, also in an image that starts later") {
    auto raw = erased(512);
    put_block(raw, 128, kVectors, FlashCipher(kTuya)); // window address 0x1000
    KeyDetection d = detect_key(raw);
    CHECK(d.kind == KeyDetection::Kind::Known);
    CHECK(d.key == kTuya);
    CHECK(d.name == "Tuya");
    CHECK(d.addr == 0x1000);
}

TEST_CASE("key detection skips blocks that are not a vector table") {
    auto raw = erased(512);
    put_block(raw, 0, kOther, FlashCipher());
    put_block(raw, 256, kVectors, FlashCipher(kTuya));
    CHECK(detect_key(raw).kind == KeyDetection::Kind::Known);
}

TEST_CASE("key detection looks only at the first block of every 4 KiB") {
    auto raw = erased(512);
    put_block(raw, 1, kVectors, FlashCipher());
    CHECK(detect_key(raw).kind == KeyDetection::Kind::Blank);
}

TEST_CASE("key detection reports an unknown key and a blank flash") {
    CHECK(detect_key(erased(512)).kind == KeyDetection::Kind::Blank);
    auto raw = erased(512);
    put_block(raw, 128, kOther, FlashCipher());
    put_block(raw, 256, kVectors, FlashCipher({1, 2, 3, 0x5A001B70}));
    KeyDetection d = detect_key(raw);
    CHECK(d.kind == KeyDetection::Kind::Unknown);
    CHECK(d.addr == 0x1000);
}
