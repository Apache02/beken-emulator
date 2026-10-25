#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "net/router/crypto.h"

using namespace bk;

namespace {

std::vector<uint8_t> bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::string hex(std::span<const uint8_t> v) {
    static const char *digits = "0123456789abcdef";
    std::string        r;
    for (uint8_t b : v) {
        r.push_back(digits[b >> 4]);
        r.push_back(digits[b & 15]);
    }
    return r;
}

std::vector<uint8_t> unhex_v(std::string_view s) {
    std::vector<uint8_t> r;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        r.push_back(static_cast<uint8_t>(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
    return r;
}

template <size_t N> std::array<uint8_t, N> unhex(std::string_view s) {
    std::array<uint8_t, N> r{};
    auto                   v = unhex_v(s);
    std::copy(v.begin(), v.end(), r.begin());
    return r;
}

} // namespace

TEST_CASE("sha1 and hmac_sha1 match the published vectors") {
    CHECK(hex(crypto::sha1(bytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(crypto::sha1(bytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    // RFC 2202 test case 1
    CHECK(hex(crypto::hmac_sha1(std::vector<uint8_t>(20, 0x0b), bytes("Hi There"))) ==
          "b617318655057264e28bc0b6fb378c8ef146be00");
}

TEST_CASE("pbkdf2 and the WPA passphrase hash match the published vectors") {
    // RFC 6070
    CHECK(hex(crypto::pbkdf2_sha1(bytes("password"), bytes("salt"), 4096, 20)) ==
          "4b007901b765489abead49d926f721d065a429c1");
    // IEEE 802.11 Annex J: passphrase "password", SSID "IEEE"
    CHECK(hex(crypto::wpa_psk("password", "IEEE")) ==
          "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
}

TEST_CASE("prf_sha1 matches the IEEE 802.11 PRF vector") {
    // IEEE 802.11 Annex J.3, PRF-512: key 0x0b x 20, label "prefix", data "Hi There".
    CHECK(hex(crypto::prf_sha1(std::vector<uint8_t>(20, 0x0b), "prefix", bytes("Hi There"), 64)) ==
          "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a");
}

TEST_CASE("aes128 and the key wrap match FIPS-197 and RFC 3394") {
    auto key = unhex<16>("000102030405060708090a0b0c0d0e0f");
    CHECK(hex(crypto::aes128_encrypt(key, unhex<16>("00112233445566778899aabbccddeeff"))) ==
          "69c4e0d86a7b0430d8cdb78070b4c55a");
    auto wrapped = crypto::aes_key_wrap(key, unhex_v("00112233445566778899aabbccddeeff"));
    CHECK(hex(wrapped) == "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    CHECK(crypto::aes_key_unwrap(key, wrapped) == unhex_v("00112233445566778899aabbccddeeff"));
    wrapped[0] ^= 1;
    CHECK_FALSE(crypto::aes_key_unwrap(key, wrapped).has_value());
    CHECK_THROWS_AS(crypto::aes_key_wrap(key, unhex_v("0011223344556677")), std::invalid_argument);
}
