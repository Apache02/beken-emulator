#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "net/router/crypto.h"

// The station side of the WPA2-PSK 4-way handshake, written straight from
// IEEE 802.11 with bk::crypto, to play against the authenticator. Frames start
// at the EAPOL version byte.
namespace wpa_test {

using Bytes = std::vector<uint8_t>;
using Mac   = std::array<uint8_t, 6>;

// Key info bits.
constexpr uint16_t kVersion2 = 0x0002, kPairwise = 0x0008, kInstall = 0x0040, kAck = 0x0080, kMic = 0x0100,
                   kSecure = 0x0200, kEncrypted = 0x1000;

// Offsets in an EAPOL-Key frame.
constexpr size_t kInfoAt = 5, kKeyLenAt = 7, kReplayAt = 9, kNonceAt = 17, kIvAt = 49, kRscAt = 65, kMicAt = 81,
                 kDataLenAt = 97, kDataAt = 99;

struct Key {
    uint8_t                 version = 0, type = 0, descriptor = 0;
    uint16_t                length = 0, info = 0, key_length = 0;
    uint64_t                replay = 0;
    std::array<uint8_t, 32> nonce{};
    std::array<uint8_t, 16> iv{}, mic{};
    std::array<uint8_t, 8>  rsc{};
    Bytes                   data;
};

inline uint64_t be(std::span<const uint8_t> b, size_t off, size_t n) {
    uint64_t x = 0;
    for (size_t i = 0; i < n; ++i) x = x << 8 | b[off + i];
    return x;
}

inline void put_be(Bytes &b, size_t off, uint64_t x, size_t n) {
    for (size_t i = 0; i < n; ++i) b[off + i] = uint8_t(x >> (8 * (n - 1 - i)));
}

inline std::optional<Key> parse(std::span<const uint8_t> f) {
    if (f.size() < kDataAt) return std::nullopt;
    Key k;
    k.version    = f[0];
    k.type       = f[1];
    k.length     = uint16_t(be(f, 2, 2));
    k.descriptor = f[4];
    k.info       = uint16_t(be(f, kInfoAt, 2));
    k.key_length = uint16_t(be(f, kKeyLenAt, 2));
    k.replay     = be(f, kReplayAt, 8);
    std::copy_n(f.begin() + kNonceAt, 32, k.nonce.begin());
    std::copy_n(f.begin() + kIvAt, 16, k.iv.begin());
    std::copy_n(f.begin() + kRscAt, 8, k.rsc.begin());
    std::copy_n(f.begin() + kMicAt, 16, k.mic.begin());
    size_t n = be(f, kDataLenAt, 2);
    if (kDataAt + n > f.size()) return std::nullopt;
    k.data.assign(f.begin() + kDataAt, f.begin() + kDataAt + n);
    return k;
}

// An EAPOL-Key frame (version 1, as wpa_supplicant sends) with a zero MIC.
inline Bytes build(uint16_t info, uint64_t replay, std::span<const uint8_t> nonce, std::span<const uint8_t> data) {
    Bytes f(kDataAt + data.size(), 0);
    f[0] = 1;
    f[1] = 3;
    put_be(f, 2, f.size() - 4, 2);
    f[4] = 2;
    put_be(f, kInfoAt, info, 2);
    put_be(f, kReplayAt, replay, 8);
    std::copy(nonce.begin(), nonce.end(), f.begin() + kNonceAt);
    put_be(f, kDataLenAt, data.size(), 2);
    std::copy(data.begin(), data.end(), f.begin() + kDataAt);
    return f;
}

// HMAC-SHA1-128 over the frame with its MIC field zeroed.
inline std::array<uint8_t, 16> mic(std::span<const uint8_t, 16> kck, std::span<const uint8_t> f) {
    Bytes z(f.begin(), f.end());
    std::fill_n(z.begin() + kMicAt, 16, 0);
    auto                    h = bk::crypto::hmac_sha1(kck, z);
    std::array<uint8_t, 16> r;
    std::copy_n(h.begin(), 16, r.begin());
    return r;
}

inline void sign(std::span<const uint8_t, 16> kck, Bytes &f) {
    auto m = mic(kck, f);
    std::copy(m.begin(), m.end(), f.begin() + kMicAt);
}

inline Bytes ptk(std::span<const uint8_t> pmk,
                 const Mac               &aa,
                 const Mac               &spa,
                 std::span<const uint8_t> anonce,
                 std::span<const uint8_t> snonce) {
    Bytes      d;
    const Mac &lo = std::min(aa, spa), &hi = std::max(aa, spa);
    d.insert(d.end(), lo.begin(), lo.end());
    d.insert(d.end(), hi.begin(), hi.end());
    bool a_first = std::lexicographical_compare(anonce.begin(), anonce.end(), snonce.begin(), snonce.end());
    auto first = a_first ? anonce : snonce, second = a_first ? snonce : anonce;
    d.insert(d.end(), first.begin(), first.end());
    d.insert(d.end(), second.begin(), second.end());
    return bk::crypto::prf_sha1(pmk, "Pairwise key expansion", d, 48);
}

// The station: answers M1 with M2 and M3 with M4.
struct Supplicant {
    std::array<uint8_t, 32> pmk;
    Mac                     aa, spa;
    std::array<uint8_t, 32> snonce;
    Bytes                   rsn; // the RSN element M2 carries
    Bytes                   ptk; // after m2()

    std::span<const uint8_t, 16> kck() const { return std::span<const uint8_t, 16>(ptk.data(), 16); }
    std::span<const uint8_t, 16> kek() const { return std::span<const uint8_t, 16>(ptk.data() + 16, 16); }
    std::span<const uint8_t, 16> tk() const { return std::span<const uint8_t, 16>(ptk.data() + 32, 16); }

    Bytes m2(const Key &m1) {
        ptk     = wpa_test::ptk(pmk, aa, spa, m1.nonce, snonce);
        Bytes f = build(kVersion2 | kPairwise | kMic, m1.replay, snonce, rsn);
        sign(kck(), f);
        return f;
    }
    Bytes m4(const Key &m3) {
        Bytes f = build(kVersion2 | kPairwise | kMic | kSecure, m3.replay, {}, {});
        sign(kck(), f);
        return f;
    }
};

} // namespace wpa_test
