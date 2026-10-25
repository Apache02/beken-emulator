#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// The primitives of the WPA2-PSK handshake: SHA-1, HMAC, PBKDF2, the IEEE 802.11
// PRF, AES-128 and the RFC 3394 key wrap. Small and unoptimized; used only while
// a station joins.
namespace bk::crypto {

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);
std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data);
std::vector<uint8_t>
pbkdf2_sha1(std::span<const uint8_t> password, std::span<const uint8_t> salt, unsigned iterations, size_t length);

// PMK of a WPA2 passphrase: PBKDF2-HMAC-SHA1, salt = SSID, 4096 iterations, 32 bytes.
std::array<uint8_t, 32> wpa_psk(std::string_view passphrase, std::string_view ssid);

// IEEE 802.11 PRF: HMAC-SHA1(key, label || 0x00 || data || counter), counter from 0,
// the outputs concatenated and truncated to length bytes.
std::vector<uint8_t>
prf_sha1(std::span<const uint8_t> key, std::string_view label, std::span<const uint8_t> data, size_t length);

std::array<uint8_t, 16> aes128_encrypt(std::span<const uint8_t, 16> key, std::span<const uint8_t, 16> block);

// RFC 3394. plain.size() must be a multiple of 8 and at least 16, else std::invalid_argument.
std::vector<uint8_t> aes_key_wrap(std::span<const uint8_t, 16> kek, std::span<const uint8_t> plain);
// nullopt when the integrity check (or the size) fails.
std::optional<std::vector<uint8_t>> aes_key_unwrap(std::span<const uint8_t, 16> kek, std::span<const uint8_t> wrapped);

} // namespace bk::crypto
