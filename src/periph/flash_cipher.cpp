#include "periph/flash_cipher.h"

#include <algorithm>
#include <cstdio>

#include "periph/crc16.h"

namespace bk {

namespace {

constexpr uint32_t kBlockRaw = 34, kBlockData = 32, kPage = 4096;
constexpr uint32_t kBranch = 0xEA000000, kLdrPc = 0xE59FF014;

struct KnownKey {
    const char *name;
    FlashKey    key;
};

// The keys images are known to be encrypted with, tried in order.
constexpr KnownKey kKnownKeys[] = {
    {"Tuya", {0x510FB093, 0xA3CBEADC, 0x5993A17E, 0xC7ADEB03}},
};

uint32_t bits(uint32_t v, unsigned hi, unsigned lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

uint32_t rotr(uint32_t v, unsigned n) { return n == 0 ? v : (v >> n) | (v << (32 - n)); }

// {a[6:0], a[15:7]} ^ (0x6371 & {4{a[8:5]}})
uint32_t pn15(uint32_t a) { return ((a & 0x7F) << 9 | bits(a, 15, 7)) ^ (0x6371 & bits(a, 8, 5) * 0x1111); }

// {a[9:0], a[16:10]} ^ (0x13659 & {a[4], {4{a[1], a[5], a[9], a[13]}}})
uint32_t pn16(uint32_t a) {
    uint32_t b = bits(a, 13, 13) | bits(a, 9, 9) << 1 | bits(a, 5, 5) << 2 | bits(a, 1, 1) << 3;
    return ((a & 0x3FF) << 7 | bits(a, 16, 10)) ^ (0x13659 & (bits(a, 4, 4) << 16 | b * 0x1111));
}

// {a[14:0], a[31:15]} ^ (0xE519A4F1 & {8{a[5:2]}})
uint32_t pn32(uint32_t a) { return rotr(a, 15) ^ (0xE519A4F1 & bits(a, 5, 2) * 0x11111111); }

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

// Whether the CRC of the 34-byte block at `b` matches its data.
bool valid(const uint8_t *b) {
    return crc16(std::span<const uint8_t>(b, kBlockData)) == (b[kBlockData] << 8 | b[kBlockData + 1]);
}

bool vector_table(const uint8_t *data, uint32_t addr, const FlashCipher &cipher) {
    for (uint32_t w = 0; w < 8; ++w) {
        uint32_t word = le32(data + 4 * w) ^ cipher.mask(addr + 4 * w);
        if (w == 0 ? (word & 0xFF000000) != kBranch : word != kLdrPc) return false;
    }
    return true;
}

} // namespace

FlashKey key_from_fuses(std::span<const uint8_t> fuses) {
    FlashKey key;
    for (size_t i = 0; i < key.size(); ++i) key[i] = le32(fuses.data() + 4 * i);
    return key;
}

std::array<uint8_t, 16> fuses_from_key(const FlashKey &key) {
    std::array<uint8_t, 16> fuses;
    for (size_t i = 0; i < fuses.size(); ++i) fuses[i] = static_cast<uint8_t>(key[i / 4] >> (8 * (i % 4)));
    return fuses;
}

std::string key_text(const FlashKey &key) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x %08x %08x %08x", key[0], key[1], key[2], key[3]);
    return buf;
}

FlashCipher::FlashCipher(const FlashKey &key) : key_(key) {
    const uint32_t coef3 = key[3];
    bypass_              = bits(coef3, 31, 24) == 0x00 || bits(coef3, 31, 24) == 0xFF || bits(coef3, 3, 0) == 0xF;
    if (bypass_) return;
    pn15_     = !bits(coef3, 0, 0);
    pn16_     = !bits(coef3, 1, 1);
    pn32_     = !bits(coef3, 2, 2);
    random_   = bits(coef3, 3, 3) ? 0 : key[2];
    pn15_sel_ = bits(coef3, 6, 5);
    pn16_sel_ = bits(coef3, 9, 8);
    pn32_sel_ = bits(coef3, 12, 11);
    // {coef1[15:8], coef3[4], coef1[7:0]}
    pn16_mix_ = bits(key[1], 15, 8) << 9 | bits(coef3, 4, 4) << 8 | bits(key[1], 7, 0);
}

uint32_t FlashCipher::mask(uint32_t addr) const {
    if (bypass_) return 0;
    uint32_t v15 = 0, v16 = 0, v32 = 0;
    if (pn15_) {
        // The two address halves, each with its bytes swapped or not.
        uint32_t hi = pn15_sel_ & 2 ? bits(addr, 23, 16) << 8 | bits(addr, 31, 24) : addr >> 16;
        uint32_t lo = pn15_sel_ & 1 ? bits(addr, 7, 0) << 8 | bits(addr, 15, 8) : addr & 0xFFFF;
        v15         = pn15((hi ^ lo) ^ bits(key_[1], 31, 16));
    }
    if (pn16_) v16 = pn16(bits(addr, 16 + pn16_sel_, pn16_sel_) ^ pn16_mix_);
    if (pn32_) v32 = pn32(rotr(addr, 8 * pn32_sel_) ^ key_[0]);
    return v32 ^ ((v15 & 0xFFFF) << 16 | (v16 & 0xFFFF)) ^ random_;
}

std::optional<uint32_t> find_vector_table(std::span<const uint8_t> raw, const FlashCipher &cipher) {
    const uint32_t blocks = static_cast<uint32_t>(raw.size() / kBlockRaw);
    for (uint32_t n = 0; n < blocks; n += kPage / kBlockData) {
        const uint8_t *b = raw.data() + static_cast<size_t>(n) * kBlockRaw;
        if (!valid(b)) continue;
        if (vector_table(b, n * kBlockData, cipher)) return n * kBlockData;
    }
    return std::nullopt;
}

KeyDetection detect_key(std::span<const uint8_t> raw) {
    if (auto addr = find_vector_table(raw, FlashCipher())) return {KeyDetection::Kind::Plain, {}, {}, *addr};
    for (const KnownKey &k : kKnownKeys)
        if (auto addr = find_vector_table(raw, FlashCipher(k.key)))
            return {KeyDetection::Kind::Known, k.key, k.name, *addr};
    const uint32_t blocks = static_cast<uint32_t>(raw.size() / kBlockRaw);
    for (uint32_t n = 0; n < blocks; n += kPage / kBlockData)
        if (valid(raw.data() + static_cast<size_t>(n) * kBlockRaw))
            return {KeyDetection::Kind::Unknown, {}, {}, n * kBlockData};
    return {};
}

} // namespace bk
