#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/machine.h"
#include "core/profile.h"
#include "periph/flash_cipher.h"

namespace bk {

// One `--efuse`: what the eFuse of the chip holds when it starts.
struct EfuseSpec {
    enum class Kind {
        File,  // the bytes of a file, an eFuse dump
        Bytes, // bytes given in hex
        Key,   // the key of the flash cipher in bytes 0..15, the rest blank
    };
    Kind                 kind = Kind::Bytes;
    std::string          file;  // File
    std::vector<uint8_t> bytes; // Bytes
    FlashKey             key{}; // Key
};

// `key:W0,W1,W2,W3` (four hex words, also separated by spaces, or 32 hex
// digits in one), HEX (an even number of hex digits, up to 64: the bytes
// from byte 0 on) or the name of a file. Throws std::invalid_argument
// `--efuse key: needs four hex words ...` or `--efuse HEX needs ...`.
EfuseSpec parse_efuse(const std::string &value);

// "file F", "bytes 93b00f51..." or "key 510fb093 a3cbeadc 5993a17e c7adeb03".
std::string describe(const EfuseSpec &spec);

// Fills the eFuse of the module SCTRL before the first run: from `spec`, or,
// without one, with the key the image in the flash is found encrypted with
// (detect_key). Then every flash takes the key of its XIP cipher from the
// eFuse, now and at every reset. A profile without SCTRL has no eFuse: its
// flashes read without the cipher.
//
// Logs what it finds, in the logger "efuse": an image encrypted with a known
// key (info), one that matches no key (warning), and an image that the eFuse
// key given by `spec` does not decrypt (warning). Throws std::invalid_argument
// `--efuse needs a profile with an SCTRL module`, `cannot read eFuse file F`
// and `eFuse file F has N bytes, the eFuse 32`.
void setup_efuse(Machine &machine, const Profile &profile, const std::optional<EfuseSpec> &spec);

} // namespace bk
