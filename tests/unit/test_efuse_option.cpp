#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "app/efuse_option.h"
#include "app/registry.h"
#include "core/log.h"
#include "core/machine.h"
#include "periph/crc16.h"
#include "periph/flash.h"
#include "periph/sctrl.h"

using namespace bk;
namespace fs = std::filesystem;

namespace {

constexpr FlashKey kTuya{0x510FB093, 0xA3CBEADC, 0x5993A17E, 0xC7ADEB03};

Profile profile(bool sctrl = true) {
    Profile p;
    p.name         = "T";
    p.insns_per_us = 100;
    p.win_base     = 0x00800000;
    p.win_size     = 0x00100000;
    p.memory.push_back({"RAM", 0x400000, 0x40000, std::nullopt, true, 0x40000});
    p.modules.push_back({
        "flash", "FLASH", {{"xip", 0}, {"regs", 0x803000}},
          {}
    });
    if (sctrl) p.modules.push_back({"sctrl", "SCTRL", {{"regs", 0x800000}}, {}});
    return p;
}

// One 34-byte block: the vector table every Beken image starts with (or
// other words), XORed with the masks of `cipher` at window address 0.
std::vector<uint8_t> first_block(const FlashCipher &cipher, bool vectors = true) {
    std::vector<uint8_t> b;
    for (uint32_t w = 0; w < 8; ++w) {
        uint32_t v = (vectors ? (w == 0 ? 0xEA00004B : 0xE59FF014) : w) ^ cipher.mask(4 * w);
        for (uint32_t i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    uint16_t crc = crc16(b);
    b.push_back(static_cast<uint8_t>(crc >> 8));
    b.push_back(static_cast<uint8_t>(crc));
    return b;
}

struct Fixture {
    std::ostringstream out;
    Profile            p;
    Machine            machine;
    Flash             &flash;

    explicit Fixture(bool sctrl = true)
        : p((Logger::reset_for_tests(), profile(sctrl))), machine(p, builtin_registry()),
          flash(dynamic_cast<Flash &>(*machine.module("FLASH"))) {
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    std::array<uint8_t, 32> fuses() {
        std::array<uint8_t, 32> f{};
        auto                    s = dynamic_cast<Sctrl &>(*machine.module("SCTRL")).efuse().fuses();
        std::copy(s.begin(), s.end(), f.begin());
        return f;
    }
};

struct TempFile {
    fs::path path = fs::temp_directory_path() / ("bkemu-efuse-" + std::to_string(::getpid()) + ".bin");
    ~TempFile() { fs::remove(path); }
    void put(const std::vector<uint8_t> &bytes) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
};

} // namespace

TEST_CASE("efuse option parses a key, hex bytes and a file") {
    EfuseSpec k = parse_efuse("key:510fb093,a3cbeadc,5993a17e,c7adeb03");
    CHECK(k.kind == EfuseSpec::Kind::Key);
    CHECK(k.key == kTuya);
    CHECK(parse_efuse("key:510fb093 a3cbeadc 5993a17e c7adeb03").key == kTuya);
    CHECK(parse_efuse("key:510fb093a3cbeadc5993a17ec7adeb03").key == kTuya);
    CHECK(parse_efuse("key:0x510fb093,0xa3cbeadc,0x5993a17e,0xc7adeb03").key == kTuya);
    CHECK(parse_efuse("key:1,2,3,4").key == FlashKey{1, 2, 3, 4});

    EfuseSpec h = parse_efuse("93b00f51");
    CHECK(h.kind == EfuseSpec::Kind::Bytes);
    CHECK(h.bytes == std::vector<uint8_t>{0x93, 0xB0, 0x0F, 0x51});
    CHECK(parse_efuse(std::string(64, 'f')).bytes.size() == 32);

    EfuseSpec f = parse_efuse("dump/efuse.bin");
    CHECK(f.kind == EfuseSpec::Kind::File);
    CHECK(f.file == "dump/efuse.bin");

    CHECK(describe(k) == "key 510fb093 a3cbeadc 5993a17e c7adeb03");
    CHECK(describe(h) == "bytes 93b00f51");
    CHECK(describe(f) == "file dump/efuse.bin");
}

TEST_CASE("efuse option errors") {
    const char *key_error = "--efuse key: needs four hex words, for example key:510fb093,a3cbeadc,5993a17e,c7adeb03";
    CHECK_THROWS_WITH_AS(parse_efuse("key:1,2,3"), key_error, std::invalid_argument);
    CHECK_THROWS_WITH_AS(parse_efuse("key:1,2,3,4,5"), key_error, std::invalid_argument);
    CHECK_THROWS_WITH_AS(parse_efuse("key:1,2,3,123456789"), key_error, std::invalid_argument);
    CHECK_THROWS_WITH_AS(parse_efuse("key:1,2,3,xyz"), key_error, std::invalid_argument);
    CHECK_THROWS_WITH_AS(parse_efuse("key:"), key_error, std::invalid_argument);
    const char *hex_error = "--efuse HEX needs an even number of hex digits, up to 64";
    CHECK_THROWS_WITH_AS(parse_efuse("abc"), hex_error, std::invalid_argument);
    CHECK_THROWS_WITH_AS(parse_efuse(std::string(66, '0')), hex_error, std::invalid_argument);
    CHECK_THROWS_AS(parse_efuse(""), std::invalid_argument);
}

TEST_CASE("efuse setup detects the Tuya key and puts it into the eFuse") {
    Fixture f;
    f.flash.load(0, first_block(FlashCipher(kTuya)));
    setup_efuse(f.machine, f.p, std::nullopt);
    auto fuses = f.fuses();
    CHECK(key_from_fuses(fuses) == kTuya);
    CHECK(fuses[16] == 0);
    CHECK(f.flash.cipher().key() == kTuya);
    CHECK(f.flash.xip_read(4, 4) == 0xE59FF014);
    CHECK(f.out.str().find("FLASH: the image at 0x00000000 is encrypted with the Tuya key 510fb093 a3cbeadc "
                           "5993a17e c7adeb03; eFuse bytes 0..15 set to it") != std::string::npos);
}

TEST_CASE("efuse setup leaves the eFuse blank for a plain image and a blank flash") {
    for (bool load : {true, false}) {
        Fixture f;
        if (load) f.flash.load(0, first_block(FlashCipher()));
        setup_efuse(f.machine, f.p, std::nullopt);
        CHECK(f.fuses() == std::array<uint8_t, 32>{});
        CHECK(f.flash.cipher().bypass());
        CHECK(f.out.str().find("[W]") == std::string::npos);
        CHECK(f.out.str().find("[I] emulator.efuse") == std::string::npos);
    }
}

TEST_CASE("efuse setup warns about an image that matches no key") {
    Fixture f;
    f.flash.load(0, first_block(FlashCipher({1, 2, 3, 0x5A001B70})));
    setup_efuse(f.machine, f.p, std::nullopt);
    CHECK(f.flash.cipher().bypass());
    CHECK(f.out.str().find("[W] emulator.efuse: FLASH: the XIP window from 0x00000000 has no vector table, plain or "
                           "under a known key; if the image is encrypted, give its key with --efuse "
                           "key:W0,W1,W2,W3") != std::string::npos);
}

TEST_CASE("efuse setup takes the key given and warns when it does not decrypt the image") {
    {
        Fixture f;
        f.flash.load(0, first_block(FlashCipher(kTuya)));
        setup_efuse(f.machine, f.p, parse_efuse("key:510fb093,a3cbeadc,5993a17e,c7adeb03"));
        CHECK(f.flash.cipher().key() == kTuya);
        CHECK(f.out.str().find("[W]") == std::string::npos);
    }
    Fixture f;
    f.flash.load(0, first_block(FlashCipher(kTuya)));
    setup_efuse(f.machine, f.p, parse_efuse("key:1,2,3,5a001b70"));
    CHECK(f.flash.cipher().key() == FlashKey{1, 2, 3, 0x5A001B70});
    CHECK(f.out.str().find("[W] emulator.efuse: FLASH: no image in the XIP window starts with a vector table under "
                           "the eFuse key 00000001 00000002 00000003 5a001b70; the firmware may not run") !=
          std::string::npos);
}

TEST_CASE("efuse setup presets every byte from hex and from a file") {
    {
        Fixture f;
        setup_efuse(f.machine, f.p, parse_efuse("00112233445566778899aabbccddeeff01"));
        CHECK(f.fuses()[15] == 0xFF);
        CHECK(f.fuses()[16] == 0x01);
        CHECK(f.fuses()[17] == 0x00);
    }
    TempFile             file;
    std::vector<uint8_t> dump(32);
    for (size_t i = 0; i < dump.size(); ++i) dump[i] = static_cast<uint8_t>(i + 1);
    file.put(dump);
    Fixture f;
    setup_efuse(f.machine, f.p, parse_efuse(file.path.string()));
    CHECK(f.fuses()[0] == 1);
    CHECK(f.fuses()[31] == 32);

    file.put(std::vector<uint8_t>(33));
    CHECK_THROWS_WITH_AS(setup_efuse(f.machine, f.p, parse_efuse(file.path.string())),
                         ("eFuse file " + file.path.string() + " has 33 bytes, the eFuse 32").c_str(),
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(setup_efuse(f.machine, f.p, parse_efuse("/nonexistent/efuse.bin")),
                         "cannot read eFuse file /nonexistent/efuse.bin",
                         std::invalid_argument);
}

TEST_CASE("efuse setup needs SCTRL for --efuse; without SCTRL the flash reads plain") {
    Fixture f(false);
    f.flash.load(0, first_block(FlashCipher(kTuya)));
    CHECK_THROWS_WITH_AS(setup_efuse(f.machine, f.p, parse_efuse("key:1,2,3,4")),
                         "--efuse needs a profile with an SCTRL module",
                         std::invalid_argument);
    setup_efuse(f.machine, f.p, std::nullopt);
    CHECK(f.flash.cipher().bypass());
}

TEST_CASE("the flash takes the eFuse key again at reset") {
    Fixture f;
    setup_efuse(f.machine, f.p, std::nullopt);
    CHECK(f.flash.cipher().bypass());
    dynamic_cast<Sctrl &>(*f.machine.module("SCTRL")).efuse().preset(fuses_from_key(kTuya));
    CHECK(f.flash.cipher().bypass());
    f.flash.reset();
    CHECK(f.flash.cipher().key() == kTuya);
}
