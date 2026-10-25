#include "net/router/crypto.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bk::crypto {

namespace {

constexpr uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

// ---- AES-128 --------------------------------------------------------------

struct AesTables {
    std::array<uint8_t, 256> sbox{};
    std::array<uint8_t, 256> inv{};
};

uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

// The S-box is derived from its definition: the multiplicative inverse in GF(2^8)
// followed by the affine transform.
const AesTables &tables() {
    static const AesTables t = [] {
        AesTables r;
        for (int x = 0; x < 256; ++x) {
            uint8_t inv = 0;
            for (int y = 1; y < 256 && x; ++y) {
                if (gmul(static_cast<uint8_t>(x), static_cast<uint8_t>(y)) == 1) {
                    inv = static_cast<uint8_t>(y);
                    break;
                }
            }
            uint8_t s = inv;
            uint8_t v = inv;
            for (int i = 0; i < 4; ++i) {
                v = static_cast<uint8_t>((v << 1) | (v >> 7));
                s ^= v;
            }
            s ^= 0x63;
            r.sbox[static_cast<size_t>(x)] = s;
            r.inv[s]                       = static_cast<uint8_t>(x);
        }
        return r;
    }();
    return t;
}

using RoundKeys = std::array<uint8_t, 176>;

RoundKeys expand_key(std::span<const uint8_t, 16> key) {
    const auto &sb = tables().sbox;
    RoundKeys   w{};
    std::copy(key.begin(), key.end(), w.begin());
    uint8_t rcon = 1;
    for (size_t i = 16; i < w.size(); i += 4) {
        uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            const uint8_t first = t[0];
            t[0]                = static_cast<uint8_t>(sb[t[1]] ^ rcon);
            t[1]                = sb[t[2]];
            t[2]                = sb[t[3]];
            t[3]                = sb[first];
            rcon                = xtime(rcon);
        }
        for (size_t k = 0; k < 4; ++k) w[i + k] = static_cast<uint8_t>(w[i - 16 + k] ^ t[k]);
    }
    return w;
}

void add_round_key(uint8_t *s, const RoundKeys &w, int round) {
    for (int i = 0; i < 16; ++i) s[i] ^= w[static_cast<size_t>(round * 16 + i)];
}

// State bytes are column-major: s[row + 4 * column].
void shift_rows(uint8_t *s, bool inverse) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) t[r + 4 * ((c + (inverse ? r : 4 - r)) % 4)] = s[r + 4 * c];
    std::memcpy(s, t, 16);
}

void mix_columns(uint8_t *s, bool inverse) {
    static constexpr uint8_t kFwd[4] = {2, 3, 1, 1};
    static constexpr uint8_t kInv[4] = {14, 11, 13, 9};
    const uint8_t           *m       = inverse ? kInv : kFwd;
    for (int c = 0; c < 4; ++c) {
        uint8_t *col = s + 4 * c;
        uint8_t  o[4];
        for (int r = 0; r < 4; ++r) {
            uint8_t v = 0;
            for (int k = 0; k < 4; ++k) v ^= gmul(col[k], m[(k + 4 - r) % 4]);
            o[r] = v;
        }
        std::memcpy(col, o, 4);
    }
}

std::array<uint8_t, 16> aes_encrypt_block(const RoundKeys &w, std::span<const uint8_t, 16> block) {
    const auto             &sb = tables().sbox;
    std::array<uint8_t, 16> s{};
    std::copy(block.begin(), block.end(), s.begin());
    add_round_key(s.data(), w, 0);
    for (int round = 1; round <= 10; ++round) {
        for (auto &b : s) b = sb[b];
        shift_rows(s.data(), false);
        if (round != 10) mix_columns(s.data(), false);
        add_round_key(s.data(), w, round);
    }
    return s;
}

std::array<uint8_t, 16> aes_decrypt_block(const RoundKeys &w, std::span<const uint8_t, 16> block) {
    const auto             &inv = tables().inv;
    std::array<uint8_t, 16> s{};
    std::copy(block.begin(), block.end(), s.begin());
    add_round_key(s.data(), w, 10);
    for (int round = 9; round >= 0; --round) {
        shift_rows(s.data(), true);
        for (auto &b : s) b = inv[b];
        add_round_key(s.data(), w, round);
        if (round != 0) mix_columns(s.data(), true);
    }
    return s;
}

// ---- RFC 3394 -------------------------------------------------------------

constexpr std::array<uint8_t, 8> kWrapIv = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};

void xor_counter(uint8_t *a, uint64_t t) {
    for (int i = 0; i < 8; ++i) a[7 - i] ^= static_cast<uint8_t>(t >> (8 * i));
}

} // namespace

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

    std::vector<uint8_t> msg(data.begin(), data.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (8 * i)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = &msg[off + static_cast<size_t>(i) * 4];
            w[i]             = static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
                   static_cast<uint32_t>(p[2]) << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e                  = d;
            d                  = c;
            c                  = rol(b, 30);
            b                  = a;
            a                  = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<uint8_t, 20> out{};
    for (size_t i = 0; i < 5; ++i)
        for (size_t k = 0; k < 4; ++k) out[i * 4 + k] = static_cast<uint8_t>(h[i] >> (24 - 8 * k));
    return out;
}

std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    std::array<uint8_t, 64> k{};
    if (key.size() > k.size()) {
        const auto d = sha1(key);
        std::copy(d.begin(), d.end(), k.begin());
    } else {
        std::copy(key.begin(), key.end(), k.begin());
    }
    std::vector<uint8_t> inner(64);
    std::vector<uint8_t> outer(64);
    for (size_t i = 0; i < 64; ++i) {
        inner[i] = k[i] ^ 0x36;
        outer[i] = k[i] ^ 0x5c;
    }
    inner.insert(inner.end(), data.begin(), data.end());
    const auto ih = sha1(inner);
    outer.insert(outer.end(), ih.begin(), ih.end());
    return sha1(outer);
}

std::vector<uint8_t>
pbkdf2_sha1(std::span<const uint8_t> password, std::span<const uint8_t> salt, unsigned iterations, size_t length) {
    std::vector<uint8_t> out;
    for (uint32_t block = 1; out.size() < length; ++block) {
        std::vector<uint8_t> msg(salt.begin(), salt.end());
        for (int i = 3; i >= 0; --i) msg.push_back(static_cast<uint8_t>(block >> (8 * i)));
        auto u = hmac_sha1(password, msg);
        auto t = u;
        for (unsigned i = 1; i < iterations; ++i) {
            u = hmac_sha1(password, u);
            for (size_t k = 0; k < t.size(); ++k) t[k] ^= u[k];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(length);
    return out;
}

std::array<uint8_t, 32> wpa_psk(std::string_view passphrase, std::string_view ssid) {
    const auto             *p = reinterpret_cast<const uint8_t *>(passphrase.data());
    const auto             *s = reinterpret_cast<const uint8_t *>(ssid.data());
    const auto              v = pbkdf2_sha1({p, passphrase.size()}, {s, ssid.size()}, 4096, 32);
    std::array<uint8_t, 32> out{};
    std::copy(v.begin(), v.end(), out.begin());
    return out;
}

std::vector<uint8_t>
prf_sha1(std::span<const uint8_t> key, std::string_view label, std::span<const uint8_t> data, size_t length) {
    std::vector<uint8_t> msg(label.begin(), label.end());
    msg.push_back(0);
    msg.insert(msg.end(), data.begin(), data.end());
    msg.push_back(0);
    std::vector<uint8_t> out;
    for (unsigned counter = 0; out.size() < length; ++counter) {
        msg.back()   = static_cast<uint8_t>(counter);
        const auto d = hmac_sha1(key, msg);
        out.insert(out.end(), d.begin(), d.end());
    }
    out.resize(length);
    return out;
}

std::array<uint8_t, 16> aes128_encrypt(std::span<const uint8_t, 16> key, std::span<const uint8_t, 16> block) {
    return aes_encrypt_block(expand_key(key), block);
}

std::vector<uint8_t> aes_key_wrap(std::span<const uint8_t, 16> kek, std::span<const uint8_t> plain) {
    if (plain.size() < 16 || plain.size() % 8 != 0) throw std::invalid_argument("aes_key_wrap: bad plaintext size");
    const RoundKeys        w = expand_key(kek);
    const size_t           n = plain.size() / 8;
    std::array<uint8_t, 8> a = kWrapIv;
    std::vector<uint8_t>   r(plain.begin(), plain.end());
    for (size_t j = 0; j < 6; ++j) {
        for (size_t i = 0; i < n; ++i) {
            std::array<uint8_t, 16> in{};
            std::copy(a.begin(), a.end(), in.begin());
            std::copy_n(&r[i * 8], 8, in.begin() + 8);
            const auto b = aes_encrypt_block(w, in);
            std::copy_n(b.begin(), 8, a.begin());
            xor_counter(a.data(), n * j + i + 1);
            std::copy_n(b.begin() + 8, 8, &r[i * 8]);
        }
    }
    std::vector<uint8_t> out(a.begin(), a.end());
    out.insert(out.end(), r.begin(), r.end());
    return out;
}

std::optional<std::vector<uint8_t>> aes_key_unwrap(std::span<const uint8_t, 16> kek, std::span<const uint8_t> wrapped) {
    if (wrapped.size() < 24 || wrapped.size() % 8 != 0) return std::nullopt;
    const RoundKeys        w = expand_key(kek);
    const size_t           n = wrapped.size() / 8 - 1;
    std::array<uint8_t, 8> a{};
    std::copy_n(wrapped.begin(), 8, a.begin());
    std::vector<uint8_t> r(wrapped.begin() + 8, wrapped.end());
    for (size_t j = 6; j-- > 0;) {
        for (size_t i = n; i-- > 0;) {
            std::array<uint8_t, 16> in{};
            std::copy(a.begin(), a.end(), in.begin());
            xor_counter(in.data(), n * j + i + 1);
            std::copy_n(&r[i * 8], 8, in.begin() + 8);
            const auto b = aes_decrypt_block(w, in);
            std::copy_n(b.begin(), 8, a.begin());
            std::copy_n(b.begin() + 8, 8, &r[i * 8]);
        }
    }
    if (a != kWrapIv) return std::nullopt;
    return r;
}

} // namespace bk::crypto
