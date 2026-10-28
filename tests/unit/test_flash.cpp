#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

#include "core/log.h"
#include "periph/crc16.h"
#include "periph/flash.h"

using namespace bk;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kOperate = 0x00, kWriteData = 0x04, kReadData = 0x08, kId = 0x10, kStatus = 0x14, kConfig = 0x1C;
constexpr uint32_t kCrcEn    = 1u << 26;
constexpr uint32_t kWindow2M = 0x1E2000; // 61680 blocks * 32, rounded up to 4 KiB

enum Op : uint32_t {
    RDID  = 20,
    READ  = 5,
    RDSR  = 3,
    RDSR2 = 6,
    WREN  = 1,
    WRDI  = 2,
    WRSR  = 4,
    WRSR2 = 7,
    PP    = 12,
    SE    = 13,
    BE1   = 14,
    BE2   = 15,
    CE    = 16
};

// Bit-by-bit CRC written independently of the implementation under test.
uint16_t bitwise_crc(const std::vector<uint8_t> &d) {
    uint32_t c = 0xFFFF;
    for (uint8_t byte : d) {
        for (int bit = 7; bit >= 0; --bit) {
            bool in  = (byte >> bit) & 1;
            bool top = (c >> 15) & 1;
            c        = (c << 1) & 0xFFFF;
            if (in != top) c ^= 0x8005;
        }
    }
    return static_cast<uint16_t>(c);
}

// One 34-byte block with data first, first+1, ... and its CRC (corrupted if !good).
std::vector<uint8_t> block(uint8_t first, bool good = true) {
    std::vector<uint8_t> b;
    for (int i = 0; i < 32; ++i) b.push_back(static_cast<uint8_t>(first + i));
    uint16_t crc = bitwise_crc(b);
    if (!good) crc ^= 1;
    b.push_back(static_cast<uint8_t>(crc >> 8));
    b.push_back(static_cast<uint8_t>(crc & 0xFF));
    return b;
}

std::vector<uint8_t> cat(std::vector<uint8_t> a, const std::vector<uint8_t> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

using Call = std::tuple<std::string, uint32_t, uint32_t>;

struct Fixture {
    std::ostringstream out;
    std::vector<Call>  inv;
    Flash              flash;

    explicit Fixture(uint32_t size = 0x200000, uint32_t jedec = 0x1C7015) : flash("flash", size, jedec) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
        flash.xip_invalidate = [this](uint32_t o, uint32_t s) { inv.emplace_back("xip", o, s); };
    }
    ~Fixture() { Logger::reset_for_tests(); }

    void     w(uint32_t off, uint32_t v) { flash.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return flash.read("regs", off, 4); }
    void     cmd(uint32_t op, uint32_t addr = 0, bool wp = true) {
        w(kOperate, addr | op << 24 | 1u << 29 | (wp ? 1u << 30 : 0));
    }
    uint32_t rdsr() {
        cmd(RDSR);
        return r(kStatus) & 0xFFFF;
    }
    void wrsr(uint32_t op, uint32_t data) {
        w(kConfig, kCrcEn | data << 10);
        cmd(op);
    }
    std::optional<Fault> xip_fault(uint32_t off, uint32_t size = 4) {
        try {
            flash.xip_read(off, size);
        } catch (const Fault &f) {
            return f;
        }
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("crc16 known") {
    std::vector<uint8_t> ff(32, 0xFF);
    CHECK(crc16(ff) == bitwise_crc(ff));
    CHECK(crc16(ff) == 0x000C);
    std::vector<uint8_t> check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16(check) == 0xAEE7); // published check value of this CRC variant
    std::vector<uint8_t> ramp = block(0);
    ramp.resize(32);
    CHECK(crc16(ramp) == bitwise_crc(ramp));
    CHECK(crc16(ramp) == 0x259D);
}

TEST_CASE("load aligns to 34") {
    Fixture f;
    CHECK(f.flash.loadable());
    CHECK_THROWS_WITH_AS(f.flash.check_load(1, 10),
                         "flash load of 10 bytes at 0x1 must start on a 34-byte block boundary",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(f.flash.check_load(0x22, 0x200000),
                         "flash load of 2097152 bytes at 0x22 does not fit the 2097152 byte flash",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(f.flash.check_load(0, 0x200001),
                         "flash load of 2097153 bytes at 0x0 does not fit the 2097152 byte flash",
                         std::invalid_argument);
    CHECK_NOTHROW(f.flash.check_load(0, 0));
    CHECK_NOTHROW(f.flash.check_load(0, 7));
    CHECK_NOTHROW(f.flash.check_load(0, 0x200000));
    CHECK_NOTHROW(f.flash.check_load(0x11000 - 0x11000 % 34, 5));
    std::vector<uint8_t> d{1, 2, 3};
    CHECK_THROWS_AS(f.flash.load(2, d), std::invalid_argument);
    CHECK(f.flash.raw() == std::vector<uint8_t>(0x200000, 0xFF));
    f.flash.load(34, d);
    std::vector<uint8_t> raw = f.flash.raw();
    CHECK(raw[33] == 0xFF);
    CHECK(raw[34] == 1);
    CHECK(raw[36] == 3);
    CHECK(raw[37] == 0xFF);
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, kWindow2M}
    });
}

TEST_CASE("xip decodes data bytes") {
    Fixture f;
    CHECK(f.flash.name() == "flash");
    // The registers are the range of the module, the window is the XIP interface.
    REQUIRE(f.flash.ranges().size() == 1);
    CHECK(f.flash.ranges()[0].name == "regs");
    CHECK(f.flash.ranges()[0].size == 0x28);
    CHECK(f.flash.xip_size() == kWindow2M);

    f.flash.load(0, cat(block(0x00), block(0x40)));
    CHECK(f.flash.xip_read(32, 4) == 0x43424140);
    CHECK(f.flash.xip_read(0, 4) == 0x03020100);
    CHECK(f.flash.xip_read(61, 1) == 0x5D);
    CHECK(f.flash.xip_read(62, 2) == 0x5F5E);
    Page p = f.flash.xip_page(0);
    CHECK(p.data[31] == 0x1F);
    CHECK(p.data[32] == 0x40);
    CHECK(p.data[63] == 0x5F);
    // Erased blocks (all 0xFF) do not carry a matching CRC.
    CHECK(p.data[64] == 0);
    REQUIRE(p.bad.size() == 126);
    CHECK(p.bad.front().start == 64);
    CHECK(p.bad.front().end == 96);
    CHECK(p.bad.back().start == 4064);
    CHECK(p.bad.back().end == 4096);
}

TEST_CASE("bad crc block is bad span and read faults") {
    Fixture f;
    f.flash.load(0, cat(cat(block(0x00), block(0x40, false)), block(0x80)));
    Page p = f.flash.xip_page(0);
    REQUIRE(p.bad.size() == 126);
    CHECK(p.bad[0].start == 32);
    CHECK(p.bad[0].end == 64);
    CHECK(p.bad[1].start == 96);
    CHECK(p.bad[1].end == 128);
    for (int i = 32; i < 64; ++i) CHECK(p.data[i] == 0);
    CHECK(p.data[64] == 0x80);

    CHECK(f.flash.xip_read(64, 4) == 0x83828180);
    auto e = f.xip_fault(36);
    REQUIRE(e);
    CHECK(e->kind == Fault::Kind::Dabt);
    CHECK(e->addr == 36);
    REQUIRE(f.xip_fault(40));
    REQUIRE(f.xip_fault(32, 1));
    CHECK(f.flash.xip_read(0, 4) == 0x03020100);
    REQUIRE(f.xip_fault(36));
    REQUIRE(f.xip_fault(100));
    REQUIRE(f.xip_fault(44));
}

TEST_CASE("crc_en clear reads bad block") {
    Fixture f;
    f.flash.load(0, cat(block(0x00), block(0x40, false)));
    CHECK(f.r(kConfig) == kCrcEn);
    f.w(kConfig, 0);
    CHECK(f.flash.xip_read(36, 4) == 0x47464544);
    CHECK(f.flash.xip_read(64, 4) == 0xFFFFFFFF);
    Page p = f.flash.xip_page(0);
    CHECK(p.bad.empty());
    CHECK(p.data[32] == 0x40);
    CHECK(p.data[64] == 0xFF);
}

TEST_CASE("read id register") {
    Fixture f;
    CHECK(f.r(kId) == 0x1C7015);
    f.cmd(RDID);
    CHECK(f.r(kId) == 0x1C7015);
    Fixture g(0x200000, 0xC84016);
    CHECK(g.r(kId) == 0xC84016);
    f.w(kId, 0);
    CHECK(f.r(kId) == 0x1C7015);
    // operate_sw reads back without busy (31) and start (29).
    f.w(kOperate, 0xFFFFFFFF & ~(0x1Fu << 24));
    CHECK(f.r(kOperate) == 0x40FFFFFF);
    CHECK(f.r(kWriteData) == 0);
    CHECK(f.r(0x0C) == 0);
    CHECK(f.r(0x18) == 0);
    CHECK(f.r(0x20) == 0);
    CHECK(f.r(0x24) == 0);
}

TEST_CASE("command read 32 bytes via fifo") {
    Fixture              f;
    std::vector<uint8_t> d;
    for (int i = 0; i < 0x50; ++i) d.push_back(static_cast<uint8_t>(i));
    f.flash.load(0, d);
    f.cmd(READ, 0x25); // raw physical address, aligned down to 0x20
    CHECK(((f.r(kStatus) >> 16) & 7) == 0);
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t b = 0x20 + 4 * i;
        CHECK(f.r(kReadData) == (b | (b + 1) << 8 | (b + 2) << 16 | (b + 3) << 24));
        CHECK(((f.r(kStatus) >> 16) & 7) == (i + 1) % 8);
    }
    CHECK(f.r(kReadData) == 0x23222120); // pointer wrapped
    f.cmd(READ, 0x40);
    CHECK(((f.r(kStatus) >> 16) & 7) == 0);
    CHECK(f.r(kReadData) == 0x43424140);
    CHECK(f.r(kReadData) == 0x47464544);
    CHECK(f.r(kReadData) == 0x4B4A4948);
    CHECK(f.r(kReadData) == 0x4F4E4D4C);
    CHECK(f.r(kReadData) == 0xFFFFFFFF);
}

TEST_CASE("rdsr and wrsr without wren") {
    Fixture f;
    CHECK(f.r(kStatus) == 7u << 19);
    f.w(kStatus, 0xABu << 22 | 0x3FFFFF);
    CHECK(f.r(kStatus) == (0xABu << 22 | 7u << 19));
    f.w(kConfig, kCrcEn | 0xA5C3u << 10);
    CHECK(f.r(kConfig) == (kCrcEn | 0xA5C3u << 10));
    f.cmd(WRSR); // wp set, no WREN
    CHECK(f.rdsr() == 0xC0);
    CHECK(f.r(kStatus) == (0xC0 | 7u << 19 | 0xABu << 22));
    f.cmd(RDSR2);
    CHECK(f.r(kStatus) == (7u << 19 | 0xABu << 22)); // WRSR leaves sr2 alone
    f.wrsr(WRSR2, 0x5A43);
    CHECK(f.rdsr() == 0x40);
    f.cmd(RDSR2);
    CHECK((f.r(kStatus) & 0xFFFF) == 0x5A);
}

TEST_CASE("explicit wren consumed") {
    Fixture f;
    CHECK(f.rdsr() == 0);
    f.cmd(WREN);
    CHECK(f.rdsr() == 0x02);
    f.cmd(WRDI);
    CHECK(f.rdsr() == 0);
    f.cmd(WREN);
    f.cmd(SE, 0x1000);
    CHECK(f.rdsr() == 0);
    f.cmd(WREN);
    f.cmd(PP, 0);
    CHECK(f.rdsr() == 0);
    f.cmd(WREN);
    f.wrsr(WRSR, 0x40);
    CHECK(f.rdsr() == 0x40);
    f.cmd(WREN);
    f.cmd(CE);
    CHECK(f.rdsr() == 0x40);
}

TEST_CASE("wp not released ignores write") {
    Fixture f;
    f.flash.load(0x1000 - 0x1000 % 34, std::vector<uint8_t>(0x200, 0));
    std::vector<uint8_t> before = f.flash.raw();
    f.inv.clear();
    f.cmd(WREN);
    f.cmd(SE, 0x1234, false);
    CHECK(f.flash.raw() == before);
    CHECK(f.rdsr() == 0x02);
    f.w(kWriteData, 0);
    f.cmd(PP, 0x40, false);
    f.cmd(PP, 0x40, false);
    f.w(kConfig, kCrcEn | 0x1Cu << 10);
    f.cmd(WRSR, 0, false);
    f.cmd(WRSR2, 0, false);
    f.cmd(BE1, 0, false);
    f.cmd(BE2, 0, false);
    f.cmd(CE, 0, false);
    CHECK(f.rdsr() == 0x02);
    CHECK(f.flash.raw() == before);
    CHECK(f.inv.empty());
    // The write FIFO was left alone: the word is still there for a released PP.
    f.cmd(PP, 0x40);
    std::vector<uint8_t> raw = f.flash.raw();
    CHECK(raw[0x40] == 0);
    CHECK(raw[0x43] == 0);
    CHECK(raw[0x44] == 0xFF);
}

TEST_CASE("pp clears bits only") {
    Fixture f;
    f.flash.load(0, std::vector<uint8_t>(34, 0xF0));
    for (int i = 0; i < 8; ++i) f.w(kWriteData, 0x3C3C3C3C);
    f.cmd(PP, 0);
    std::vector<uint8_t> raw = f.flash.raw();
    for (int i = 0; i < 32; ++i) CHECK(raw[i] == 0x30);
    CHECK(raw[32] == 0xF0); // CRC bytes are outside the 32 programmed bytes
    CHECK(raw[33] == 0xF0);
    // The FIFO was cleared: an empty FIFO programs all ones, changing nothing.
    f.cmd(PP, 0);
    CHECK(f.flash.raw() == raw);
    f.w(kWriteData, 0x12345678);
    f.cmd(PP, 0x47); // aligned down to 0x40
    raw = f.flash.raw();
    CHECK(raw[0x40] == 0x78);
    CHECK(raw[0x41] == 0x56);
    CHECK(raw[0x42] == 0x34);
    CHECK(raw[0x43] == 0x12);
    CHECK(raw[0x44] == 0xFF);
    CHECK(raw[0x5F] == 0xFF);
    CHECK(raw[0x60] == 0xFF);
}

TEST_CASE("write fifo keeps the last 8 words and pads with ones") {
    Fixture f;
    f.flash.load(0, std::vector<uint8_t>(0x80, 0xFF));
    for (uint32_t i = 1; i <= 10; ++i) f.w(kWriteData, i * 0x01010101u);
    f.cmd(PP, 0);
    std::vector<uint8_t> raw = f.flash.raw();
    for (uint32_t i = 0; i < 32; ++i) CHECK(raw[i] == 3 + i / 4);
    for (uint32_t v : {0xA1A2A3A4u, 0xB1B2B3B4u, 0xC1C2C3C4u}) f.w(kWriteData, v);
    f.cmd(PP, 0x20);
    raw = f.flash.raw();
    CHECK(raw[0x20] == 0xA4);
    CHECK(raw[0x23] == 0xA1);
    CHECK(raw[0x24] == 0xB4);
    CHECK(raw[0x28] == 0xC4);
    CHECK(raw[0x2B] == 0xC1);
    for (int i = 0x2C; i < 0x40; ++i) CHECK(raw[i] == 0xFF);
}

TEST_CASE("pp clips to the flash size") {
    Fixture f(100);
    f.flash.load(0, std::vector<uint8_t>(100, 0xFF));
    f.inv.clear();
    for (int i = 0; i < 8; ++i) f.w(kWriteData, 0);
    f.cmd(PP, 0x63); // aligned to 0x60; only 0x60..0x63 exist
    std::vector<uint8_t> raw = f.flash.raw();
    REQUIRE(raw.size() == 100);
    for (int i = 0; i < 0x60; ++i) CHECK(raw[i] == 0xFF);
    for (int i = 0x60; i < 100; ++i) CHECK(raw[i] == 0);
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 64, 32}
    });
    f.inv.clear();
    f.w(kWriteData, 0);
    f.cmd(PP, 0x80); // entirely past the end
    CHECK(f.flash.raw() == raw);
    CHECK(f.inv.empty());
    CHECK(f.rdsr() == 0);
}

TEST_CASE("the xip window is limited to the first 2 MiB") {
    // A 4 MiB flash: the window stops at 2 MiB, the rest is read through the registers.
    Flash big("flash", 0x400000);
    CHECK(big.xip_size() == 0x200000);
    // A smaller flash keeps the window of its data.
    Flash small("flash", 0x200000);
    CHECK(small.xip_size() == kWindow2M);
}

TEST_CASE("xip_size sets the window, a multiple of 4096 up to 2 MiB") {
    Flash::Options o;
    o.xip_size = 0x100000;
    Flash flash("flash", 0x200000, 0x1C7015, o);
    CHECK(flash.xip_size() == 0x100000);
    // Reads past the data of the flash fail even inside a bigger window.
    o.xip_size = 0x200000;
    Flash wide("flash", 0x200000, 0x1C7015, o);
    CHECK(wide.xip_size() == 0x200000);
    for (uint32_t bad : {0x100u, 0x1001u, 0x201000u, 0x400000u}) {
        o.xip_size = bad;
        CHECK_THROWS_WITH_AS(Flash("flash", 0x200000, 0x1C7015, o),
                             "flash: xip_size must be a multiple of 4096 up to 0x200000",
                             std::invalid_argument);
    }
}

TEST_CASE("size not a multiple of 34") {
    Fixture f(100); // 2 blocks, 32 trailing bytes
    CHECK(f.flash.xip_size() == 4096);
    CHECK_NOTHROW(f.flash.check_load(68, 32));
    CHECK_THROWS_WITH_AS(f.flash.check_load(68, 33),
                         "flash load of 33 bytes at 0x44 does not fit the 100 byte flash",
                         std::invalid_argument);
    f.flash.load(0, cat(cat(block(0x00), block(0x40)), std::vector<uint8_t>(32, 0x11)));
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, 4096}
    });
    CHECK(f.flash.xip_read(60, 4) == 0x5F5E5D5C);
    f.w(kConfig, 0);
    auto e = f.xip_fault(64);
    REQUIRE(e);
    CHECK(e->addr == 64);
    Page p = f.flash.xip_page(0);
    REQUIRE(p.bad.size() == 126);
    CHECK(p.bad[0].start == 64);
    CHECK(p.data[64] == 0);
    CHECK(p.data[63] == 0x5F);
    f.cmd(READ, 0x60);
    CHECK(f.r(kReadData) == 0x11111111);
    CHECK(f.r(kReadData) == 0xFFFFFFFF);
}

TEST_CASE("xip read past blocks always faults") {
    Fixture f;
    f.w(kConfig, 0);
    CHECK(f.flash.xip_read(0x1E1DFC, 4) == 0xFFFFFFFF);
    auto e = f.xip_fault(0x1E1E00);
    REQUIRE(e);
    CHECK(e->kind == Fault::Kind::Dabt);
    CHECK(e->addr == 0x1E1E00);
    REQUIRE(f.xip_fault(0x1E1FFC));
    Page p = f.flash.xip_page(0x1E1000);
    REQUIRE(p.bad.size() == 16);
    CHECK(p.bad[0].start == 0xE00);
    CHECK(p.bad[0].end == 0xE20);
    CHECK(p.bad[15].start == 0xFE0);
    CHECK(p.bad[15].end == 0x1000);
    CHECK(p.data[0xDFF] == 0xFF);
    CHECK(p.data[0xE00] == 0);
}

TEST_CASE("load forgets verdicts of whole blocks only") {
    Fixture f;
    f.flash.load(0, cat(block(0x00), block(0x40)));
    CHECK(f.flash.xip_read(0, 4) == 0x03020100); // verdict cached: good
    CHECK(f.flash.xip_read(32, 4) == 0x43424140);
    // A partial block keeps its cached verdict although its CRC no longer matches.
    f.flash.load(0, std::vector<uint8_t>(20, 0xEE));
    CHECK(f.flash.xip_read(0, 4) == 0xEEEEEEEE);
    // Covering block 1 completely forgets its verdict.
    f.flash.load(34, cat(std::vector<uint8_t>(32, 0x77), {0, 0}));
    CHECK(f.xip_fault(32));
    CHECK(f.flash.xip_read(16, 4) == 0xEEEEEEEE);
}

TEST_CASE("erase sector 4 KiB, block 32/64 KiB, chip") {
    Fixture f;
    f.flash.load(0, std::vector<uint8_t>(0x200000, 0));
    f.cmd(SE, 0x1234);
    std::vector<uint8_t> raw = f.flash.raw();
    CHECK(raw[0xFFF] == 0);
    CHECK(raw[0x1000] == 0xFF);
    CHECK(raw[0x1FFF] == 0xFF);
    CHECK(raw[0x2000] == 0);
    f.cmd(BE1, 0x9ABC);
    raw = f.flash.raw();
    CHECK(raw[0x7FFF] == 0);
    CHECK(raw[0x8000] == 0xFF);
    CHECK(raw[0xFFFF] == 0xFF);
    CHECK(raw[0x10000] == 0);
    f.cmd(BE2, 0x2ABCD);
    raw = f.flash.raw();
    CHECK(raw[0x1FFFF] == 0);
    CHECK(raw[0x20000] == 0xFF);
    CHECK(raw[0x2FFFF] == 0xFF);
    CHECK(raw[0x30000] == 0);
    size_t erased = 0;
    for (uint8_t b : raw) erased += b == 0xFF;
    CHECK(erased == 0x1000 + 0x8000 + 0x10000);
    f.cmd(CE);
    CHECK(f.flash.raw() == std::vector<uint8_t>(0x200000, 0xFF));

    Fixture g(0x1800); // a sector clipped by the flash size
    g.flash.load(0, std::vector<uint8_t>(0x1800, 0));
    g.cmd(SE, 0x1700);
    raw = g.flash.raw();
    REQUIRE(raw.size() == 0x1800);
    CHECK(raw[0xFFF] == 0);
    for (size_t i = 0x1000; i < 0x1800; ++i) CHECK(raw[i] == 0xFF);
    g.cmd(SE, 0x2000); // entirely past the end
    CHECK(g.flash.raw() == raw);
}

TEST_CASE("bp bits suppress pp and ignore erase, wrsr allowed") {
    Fixture f;
    f.flash.load(0, std::vector<uint8_t>(0x2000, 0));
    std::vector<uint8_t> before = f.flash.raw();
    f.inv.clear();
    f.wrsr(WRSR, 0x1C);
    CHECK(f.rdsr() == 0x1C);
    f.cmd(WREN);
    f.w(kWriteData, 0x12345678);
    f.cmd(PP, 0x2040);
    CHECK(f.rdsr() == 0x1E); // WEL not consumed
    for (uint32_t op : {SE, BE1, BE2, CE}) f.cmd(op, 0x1000);
    CHECK(f.rdsr() == 0x1C); // the first erase consumed WEL
    CHECK(f.flash.raw() == before);
    CHECK(f.inv.empty());
    f.cmd(WREN);
    f.wrsr(WRSR2, 0x0400); // status write allowed while protected; consumes WEL
    CHECK(f.rdsr() == 0);
    f.cmd(RDSR2);
    CHECK((f.r(kStatus) & 0xFF) == 0x04);
    // Unprotected now: the FIFO word kept by the suppressed PP is programmed.
    f.cmd(PP, 0x2040);
    f.cmd(SE, 0x1000);
    std::vector<uint8_t> raw = f.flash.raw();
    CHECK(raw[0x2040] == 0x78);
    CHECK(raw[0x2043] == 0x12);
    CHECK(raw[0x2044] == 0xFF);
    CHECK(raw[0xFFF] == 0);
    CHECK(raw[0x1000] == 0xFF);
}

TEST_CASE("erase invalidates xip") {
    Fixture f;
    f.flash.load(121 * 34, block(0x10));
    f.inv.clear();
    CHECK(f.flash.xip_read(121 * 32, 4) == 0x13121110); // verdict cached
    f.cmd(SE, 0x1000);
    // Raw 0x1000..0x1FFF touches blocks 120..240 -> logical 120*32 .. 241*32.
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 3840, 3872}
    });
    CHECK(f.xip_fault(121 * 32)); // verdict forgotten, block is now erased
    f.inv.clear();
    f.cmd(PP, 0x40); // raw 0x40..0x5F: blocks 1..2
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 32, 64}
    });
    f.inv.clear();
    f.cmd(CE); // all blocks including the trailing partial one
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, 61681 * 32}
    });
    f.inv.clear();
    f.cmd(BE2, 0x1F0000); // clipped at the end of the flash
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0x1F0000 / 34 * 32, (61681 - 0x1F0000 / 34) * 32}
    });
}

TEST_CASE("crc_en toggling invalidates the window") {
    Fixture f;
    f.w(kConfig, kCrcEn | 0x55u << 10);
    CHECK(f.inv.empty());
    f.w(kConfig, 0x55u << 10);
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, kWindow2M}
    });
    f.w(kConfig, 0);
    CHECK(f.inv.size() == 1);
    f.w(kConfig, kCrcEn);
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, kWindow2M},
                       {"xip", 0, kWindow2M}
    });
    f.inv.clear();
    f.flash.reset();
    CHECK(f.inv.empty()); // crc_en unchanged by reset
    f.w(kConfig, 0);
    f.inv.clear();
    f.flash.reset();
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, kWindow2M}
    });
    CHECK(f.r(kConfig) == kCrcEn);
}

TEST_CASE("reset keeps contents") {
    Fixture f;
    f.flash.load(0, cat(block(0x00), block(0x40)));
    f.wrsr(WRSR2, 0x2040);
    f.cmd(WREN);
    f.w(kStatus, 0x12u << 22);
    f.w(kWriteData, 0);
    f.w(kConfig, 0x77u << 10);
    f.cmd(READ, 0);
    CHECK(f.r(kReadData) == 0x03020100);
    std::vector<uint8_t> before = f.flash.raw();
    f.inv.clear();

    f.flash.reset();
    CHECK(f.flash.raw() == before);
    CHECK(f.inv == std::vector<Call>{
                       {"xip", 0, kWindow2M}
    });
    CHECK(f.r(kOperate) == 0);
    CHECK(f.r(kConfig) == kCrcEn);
    CHECK(f.r(kStatus) == 7u << 19);
    CHECK(f.r(kReadData) == 0); // read FIFO emptied
    CHECK(((f.r(kStatus) >> 16) & 7) == 1);
    CHECK(f.rdsr() == 0);
    f.cmd(RDSR2);
    CHECK((f.r(kStatus) & 0xFF) == 0);
    f.cmd(PP, 0x40); // write FIFO emptied: programs all ones
    CHECK(f.flash.raw() == before);
    CHECK(f.flash.xip_read(32, 4) == 0x43424140);
}

namespace {

// The controller as a running chip is measured: clk_conf 15, the quad-mode M byte 0xa0 and an
// unused non-zero word.
Flash::Options measured() {
    Flash::Options o;
    o.words = {
        {0x14, 0xA0u << 22 | 7u << 19 | 1u << 16},
        {0x1C, Flash::kCrcEn | 0xF              },
        {0x20, 0x01FFFFFF                       },
    };
    return o;
}

} // namespace

TEST_CASE("controller start state can be given and is what reset returns to") {
    Flash flash("flash", 0x200000, 0x1C7015, measured());
    auto  r = [&](uint32_t off) { return flash.read("regs", off, 4); };
    auto  w = [&](uint32_t off, uint32_t v) { flash.write("regs", off, 4, v); };
    CHECK(r(kConfig) == 0x0400000F);
    CHECK(r(kStatus) == (0xA0u << 22 | 7u << 19 | 1u << 16));
    CHECK(r(0x20) == 0x01FFFFFF);
    // Everything the firmware may change goes back at reset.
    w(kConfig, kCrcEn);
    w(kStatus, 0x11u << 22);
    w(0x20, 5);
    r(kReadData); // moves the read pointer
    CHECK(r(0x20) == 5);
    flash.reset();
    CHECK(r(kConfig) == 0x0400000F);
    CHECK(r(kStatus) == (0xA0u << 22 | 7u << 19 | 1u << 16));
    CHECK(r(0x20) == 0x01FFFFFF);
}

TEST_CASE("the read FIFO pointer is the field [18:16] of the status start value and a read command starts over at 0") {
    Flash::Options o;
    o.words = {
        {0x14, 7u << 16}
    };
    Flash flash("flash", 0x200000, 0x1C7015, o);
    CHECK(((flash.read("regs", kStatus, 4) >> 16) & 7) == 7);
    flash.write("regs", kOperate, 4, 5u << 24 | 1u << 29 | 1u << 30); // READ
    CHECK(((flash.read("regs", kStatus, 4) >> 16) & 7) == 0);
}

TEST_CASE("controller words that are not given read 0 and ignore writes") {
    Flash flash("flash", 0x200000, 0x1C7015, measured());
    flash.write("regs", 0x24, 4, 0x1234);
    flash.write("regs", 0x0C, 4, 0x1234);
    flash.write("regs", 0x18, 4, 0x1234);
    CHECK(flash.read("regs", 0x24, 4) == 0);
    CHECK(flash.read("regs", 0x0C, 4) == 0);
    CHECK(flash.read("regs", 0x18, 4) == 0);
    Flash plain("flash");
    CHECK(plain.read("regs", 0x20, 4) == 0);
    CHECK(plain.read("regs", kConfig, 4) == Flash::kCrcEn);
    CHECK(plain.read("regs", kStatus, 4) == 7u << 19);
}

TEST_CASE("a start state without crc_en reads bad blocks, reset keeps it") {
    Flash::Options o;
    o.words = {
        {0x1C, 0}
    };
    Flash flash("flash", 0x200000, 0x1C7015, o);
    // A block with a wrong CRC reads its data while crc_en is clear.
    std::vector<uint8_t> block(34, 0x11);
    flash.load(0, block);
    CHECK(flash.xip_read(0, 4) == 0x11111111);
    flash.write("regs", kConfig, 4, kCrcEn);
    CHECK_THROWS_AS(flash.xip_read(0, 4), Fault);
    flash.reset();
    CHECK(flash.read("regs", kConfig, 4) == 0);
    CHECK(flash.xip_read(0, 4) == 0x11111111);
}

TEST_CASE("controller words must be config, status or plain word offsets of the regs range") {
    for (uint32_t offset : {0x00u, 0x04u, 0x08u, 0x10u, 0x28u, 0x100u, 0x21u, 0x22u}) {
        Flash::Options o;
        o.words = {
            {offset, 1}
        };
        CHECK_THROWS_AS(Flash("flash", 0x200000, 0x1C7015, o), std::invalid_argument);
    }
    for (uint32_t offset : {0x0Cu, 0x14u, 0x18u, 0x1Cu, 0x20u, 0x24u}) {
        Flash::Options o;
        o.words = {
            {offset, 1}
        };
        CHECK_NOTHROW(Flash("flash", 0x200000, 0x1C7015, o));
    }
}

TEST_CASE("load rejects data larger than the flash") {
    Fixture f;
    CHECK_THROWS_AS(f.flash.check_load(0, UINT32_MAX), std::invalid_argument);
    CHECK_THROWS_AS(f.flash.check_load(0x22, UINT32_MAX), std::invalid_argument);
    std::vector<uint8_t> over(0x200001, 0x55);
    CHECK_THROWS_AS(f.flash.load(0, over), std::invalid_argument);
    // A size beyond 32 bits is rejected before the buffer is touched.
    const uint8_t            b = 0;
    std::span<const uint8_t> huge(&b, (size_t{1} << 32) + 4);
    CHECK_THROWS_AS(f.flash.load(0, huge), std::invalid_argument);
    CHECK(f.flash.raw() == std::vector<uint8_t>(0x200000, 0xFF));
}

namespace {

struct TempFile {
    fs::path path;
    TempFile() {
        static int counter = 0;
        path               = fs::temp_directory_path() /
               ("bk7252-flash-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + ".bin");
        fs::remove(path);
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    std::vector<uint8_t> bytes() const {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    void put(const std::vector<uint8_t> &data) const {
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
};

constexpr uint32_t kSmall = 4 * 34;

} // namespace

TEST_CASE("mount creates a missing file with the present contents") {
    TempFile file;
    Fixture  f(kSmall);
    f.flash.load(0, block(0x10));
    f.flash.mount(file.path.string());
    REQUIRE(f.flash.mounted());
    CHECK(f.flash.mounted()->path() == file.path.string());
    CHECK(fs::file_size(file.path) == kSmall);
    CHECK(file.bytes() == f.flash.raw());
    CHECK(f.flash.raw() == cat(block(0x10), std::vector<uint8_t>(3 * 34, 0xFF)));
    // The whole XIP window is invalidated.
    CHECK(f.inv.back() == Call{"xip", 0, 0x1000});
}

TEST_CASE("mount of an existing file replaces the contents") {
    TempFile             file;
    std::vector<uint8_t> image = cat(cat(block(0x20), block(0x40, false)), std::vector<uint8_t>(2 * 34, 0x00));
    file.put(image);
    Fixture f(kSmall);
    f.flash.load(0, block(0x10));
    CHECK(f.flash.xip_read(0, 4) == 0x13121110);
    f.flash.mount(file.path.string());
    CHECK(f.flash.raw() == image);
    CHECK(file.bytes() == image);
    // Cached verdicts are forgotten: block 0 reads the file, block 1 has a bad CRC.
    CHECK(f.flash.xip_read(0, 4) == 0x23222120);
    CHECK(f.xip_fault(32).has_value());
}

TEST_CASE("loads, programs and erases reach the mounted file at once") {
    TempFile file;
    Fixture  f(kSmall);
    f.flash.mount(file.path.string());
    f.flash.load(34, block(0x30));
    std::vector<uint8_t> bytes = file.bytes();
    CHECK(bytes == f.flash.raw());
    CHECK(std::vector<uint8_t>(bytes.begin() + 34, bytes.begin() + 68) == block(0x30));
    for (int i = 0; i < 8; ++i) f.w(kWriteData, 0);
    f.cmd(PP, 0);
    CHECK(file.bytes()[0] == 0x00);
    CHECK(file.bytes() == f.flash.raw());
    f.cmd(SE, 0);
    CHECK(file.bytes() == std::vector<uint8_t>(kSmall, 0xFF));
}

TEST_CASE("the mounted file keeps the contents for the next mount") {
    TempFile file;
    {
        Fixture f(kSmall);
        f.flash.mount(file.path.string());
        f.flash.load(68, block(0x50));
    }
    Fixture f(kSmall);
    f.flash.mount(file.path.string());
    std::vector<uint8_t> raw = f.flash.raw();
    CHECK(std::vector<uint8_t>(raw.begin() + 68, raw.begin() + 102) == block(0x50));
}

TEST_CASE("a load that changes nothing in the mounted file does not invalidate xip") {
    TempFile file;
    Fixture  f(kSmall);
    f.flash.mount(file.path.string());
    f.flash.load(34, block(0x30));
    f.inv.clear();
    f.flash.load(34, block(0x30));
    CHECK(f.inv.empty());
}

TEST_CASE("mount refuses a file of another size, one in use and a second mount") {
    TempFile file;
    file.put(std::vector<uint8_t>(kSmall + 1, 0xFF));
    Fixture f(kSmall);
    CHECK_THROWS_WITH_AS(f.flash.mount(file.path.string()),
                         (file.path.string() + ": is 137 bytes, the flash is 136").c_str(),
                         std::runtime_error);
    CHECK_FALSE(f.flash.mounted());
    CHECK(fs::file_size(file.path) == kSmall + 1);

    TempFile other;
    f.flash.mount(other.path.string());
    CHECK_THROWS_AS(f.flash.mount(file.path.string()), std::logic_error);
    Fixture second(kSmall);
    CHECK_THROWS_WITH_AS(second.flash.mount(other.path.string()),
                         (other.path.string() + ": is already in use").c_str(),
                         std::runtime_error);
    CHECK_THROWS_AS(second.flash.mount("/nonexistent-dir/flash.bin"), std::runtime_error);
}
