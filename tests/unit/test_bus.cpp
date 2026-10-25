#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "bus/bus.h"
#include "core/log.h"
#include "cpu/cpu.h"
#include "periph/crc16.h"
#include "periph/flash.h"

using namespace bk;

namespace {

constexpr uint32_t kSys = 0x1F, kAbt = 0x17, kI = 0x80;

// ARM encodings.
constexpr uint32_t kLdrR1R2  = 0xE5921000; // ldr r1, [r2]
constexpr uint32_t kLdrR3R2  = 0xE5923000; // ldr r3, [r2]
constexpr uint32_t kLdrbR1R2 = 0xE5D21000; // ldrb r1, [r2]
constexpr uint32_t kLdrhR3R4 = 0xE1D430B0; // ldrh r3, [r4]
constexpr uint32_t kLdrR1R4  = 0xE5941000; // ldr r1, [r4]
constexpr uint32_t kLdrR3R4  = 0xE5943000; // ldr r3, [r4]
constexpr uint32_t kStrR1R2  = 0xE5821000; // str r1, [r2]
constexpr uint32_t kStrbR1R2 = 0xE5C21000; // strb r1, [r2]
constexpr uint32_t kStrhR1R2 = 0xE1C210B0; // strh r1, [r2]
constexpr uint32_t kStrR1R4  = 0xE5841000; // str r1, [r4]
constexpr uint32_t kBxR2     = 0xE12FFF12; // bx r2
constexpr uint32_t mov_r0(uint8_t imm) { return 0xE3A00000 | imm; }
constexpr uint32_t b(uint32_t from, uint32_t to) { return 0xEA000000 | (((to - from - 8) >> 2) & 0x00FFFFFF); }

constexpr uint32_t kWin = 0x00800000, kWinSize = 0x00100000;
constexpr uint32_t kRegs = 0x00800000, kOther = 0x00800020, kMac = 0x00A00100;
constexpr uint32_t kData = 0x00400000; // non-executable RAM
constexpr uint32_t kRom  = 0x00100000; // demand range of the fake rom

using Call = std::tuple<std::string, std::string, uint32_t, uint32_t, uint32_t>;

// Register-style module: records every access.
struct FakeDev : Module {
    std::string                     id;
    std::vector<Call>               calls;
    std::map<std::string, uint32_t> value; // read result per range
    bool                            fault = false;

    explicit FakeDev(std::string n = "dev") : id(std::move(n)) {}
    std::string        name() const override { return id; }
    std::vector<Range> ranges() const override {
        return {
            {"regs",  0x20 },
            {"other", 0x10 },
            {"mac",   0x100}
        };
    }
    uint32_t read(const std::string &r, uint32_t off, uint32_t size) override {
        calls.emplace_back("read", r, off, size, 0);
        if (fault) throw Fault{Fault::Kind::Dabt, off};
        return value[r];
    }
    void write(const std::string &r, uint32_t off, uint32_t size, uint32_t v) override {
        calls.emplace_back("write", r, off, size, v);
    }
};

// Demand-range module: 8 KiB of bytes plus bad spans per page.
struct FakeRom : Module {
    std::vector<uint8_t>                     mem = std::vector<uint8_t>(0x2000);
    std::map<uint32_t, std::vector<BadSpan>> bad;   // by page offset
    std::vector<uint32_t>                    pages; // page() calls
    std::vector<Call>                        calls; // read() calls

    FakeRom() {
        for (size_t i = 0; i < mem.size(); ++i) mem[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    std::string        name() const override { return "rom"; }
    std::vector<Range> demand_ranges() const override {
        return {
            {"code", 0x2000}
        };
    }
    std::vector<Range> ranges() const override {
        return {
            {"code", 0x2000}
        };
    }
    bool is_bad(uint32_t off, uint32_t size) const {
        auto it = bad.find(off & ~0xFFFu);
        if (it == bad.end()) return false;
        for (const BadSpan &s : it->second)
            if ((off & 0xFFF) < s.end && s.start < (off & 0xFFF) + size) return true;
        return false;
    }
    uint32_t read(const std::string &r, uint32_t off, uint32_t size) override {
        calls.emplace_back("read", r, off, size, 0);
        if (is_bad(off, size)) throw Fault{Fault::Kind::Dabt, off};
        uint32_t v = 0;
        for (uint32_t i = 0; i < size; ++i) v |= static_cast<uint32_t>(mem[off + i]) << (8 * i);
        return v;
    }
    void write(const std::string &, uint32_t, uint32_t, uint32_t) override {}
    Page page(const std::string &r, uint32_t off) override {
        REQUIRE(r == "code");
        pages.push_back(off);
        Page p{};
        std::copy_n(mem.begin() + off, 4096, p.data.begin());
        if (bad.count(off)) p.bad = bad[off];
        return p;
    }
    uint32_t word(uint32_t off) const {
        return mem[off] | mem[off + 1] << 8 | mem[off + 2] << 16 | static_cast<uint32_t>(mem[off + 3]) << 24;
    }
    void put(uint32_t off, uint32_t w) {
        for (int i = 0; i < 4; ++i) mem[off + i] = static_cast<uint8_t>(w >> (8 * i));
    }
};

using Result = std::pair<RunResult, uint32_t>;

struct Fixture {
    std::ostringstream out;
    Cpu                cpu;
    Bus                bus{cpu};
    FakeDev            dev;
    FakeRom            rom;

    Fixture() {
        Logger::reset_for_tests();
        Logger::configure(out, false);
        bus.add_memory("itcm", 0, 0x10000, true);
        bus.add_memory("data", kData, 0x10000, false);
        bus.claim(dev,
                  {
                      {"regs",  kRegs },
                      {"other", kOther},
                      {"mac",   kMac  }
        });
        bus.claim(rom,
                  {
                      {"code", kRom}
        });
        bus.claim_unclaimed(kWin, kWinSize);
        // Vector table: every vector branches to its own parking loop.
        for (uint32_t v = 0; v < 0x20; v += 4) {
            put(v, b(v, handler(v)));
            put(handler(v), b(handler(v), handler(v)));
        }
        start();
    }
    ~Fixture() { Logger::reset_for_tests(); }

    static uint32_t handler(uint32_t vector) { return 0x1000 + vector * 0x40; }

    void start(uint32_t pc = 0x100) {
        cpu.reset(pc);
        cpu.set_reg(Reg::CPSR, kSys);
    }
    void put(uint32_t addr, uint32_t word) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(word),
                                  static_cast<uint8_t>(word >> 8),
                                  static_cast<uint8_t>(word >> 16),
                                  static_cast<uint8_t>(word >> 24)};
        cpu.mem_write(addr, bytes);
    }
    // Places `code` at 0x100 followed by a parking loop.
    void program(std::vector<uint32_t> code) {
        uint32_t a = 0x100;
        for (uint32_t w : code) put(a, w), a += 4;
        put(a, b(a, a));
        // Writing memory does not drop code translated from it earlier.
        REQUIRE(uc_ctl_flush_tb(cpu.engine()) == UC_ERR_OK);
        start();
    }
    Result run(uint32_t n = 100) {
        try {
            return cpu.run(n);
        } catch (const std::runtime_error &e) {
            MESSAGE("run failed: ", e.what());
            return {RunResult::Stopped, 0xDEAD};
        }
    }
    uint32_t r(Reg reg) { return cpu.reg(reg); }
    bool     mapped(uint32_t addr) {
        try {
            cpu.mem_read(addr, 1);
            return true;
        } catch (const std::runtime_error &) {
            return false;
        }
    }
};

const Result kOk{RunResult::Budget, 100};

} // namespace

TEST_CASE("mmio read dispatches size and offset") {
    Fixture f;
    f.dev.value["regs"] = 0x12345678;
    f.program({kLdrR1R2});
    f.cpu.set_reg(Reg::R2, 0x00800004);
    CHECK(f.run() == kOk);
    CHECK(f.dev.calls == std::vector<Call>{
                             {"read", "regs", 4, 4, 0}
    });
    CHECK(f.r(Reg::R1) == 0x12345678);
    CHECK(f.r(Reg::PC) == 0x104);
}

TEST_CASE("mmio byte and halfword reads take their bytes from the word the module returns") {
    Fixture f;
    f.dev.value["other"] = 0x11223344;
    f.dev.value["regs"]  = 0x55667788;
    f.program({kLdrbR1R2, kLdrhR3R4});
    f.cpu.set_reg(Reg::R2, kOther + 1);
    f.cpu.set_reg(Reg::R4, kRegs + 6);
    CHECK(f.run() == kOk);
    // One word read per access, at the word address.
    CHECK(f.dev.calls == std::vector<Call>{
                             {"read", "other", 0, 4, 0},
                             {"read", "regs",  4, 4, 0}
    });
    CHECK(f.r(Reg::R1) == 0x33);
    CHECK(f.r(Reg::R3) == 0x5566);
}

TEST_CASE("mmio narrow reads see every lane of the word") {
    for (uint32_t lane = 0; lane < 4; ++lane) {
        Fixture f;
        f.dev.value["regs"] = 0x11223344;
        f.program({kLdrbR1R2});
        f.cpu.set_reg(Reg::R2, kRegs + lane);
        CHECK(f.run() == kOk);
        CHECK(f.r(Reg::R1) == (0x11223344u >> (8 * lane) & 0xFF));
    }
}

TEST_CASE("mmio write dispatch") {
    Fixture f;
    f.program({kStrR1R2, kStrbR1R2, kStrhR1R2});
    f.cpu.set_reg(Reg::R1, 0xCAFEBABE);
    SUBCASE("word, byte and halfword to one range") {
        f.cpu.set_reg(Reg::R2, kRegs + 8);
        CHECK(f.run() == kOk);
        CHECK(f.dev.calls == std::vector<Call>{
                                 {"write", "regs", 8, 4, 0xCAFEBABE},
                                 {"write", "regs", 8, 4, 0xBEBEBEBE},
                                 {"write", "regs", 8, 4, 0xBABEBABE}
        });
    }
    SUBCASE("adjacent range gets its own offsets") {
        f.cpu.set_reg(Reg::R2, kOther + 4);
        CHECK(f.run() == kOk);
        CHECK(f.dev.calls == std::vector<Call>{
                                 {"write", "other", 4, 4, 0xCAFEBABE},
                                 {"write", "other", 4, 4, 0xBEBEBEBE},
                                 {"write", "other", 4, 4, 0xBABEBABE}
        });
    }
}

TEST_CASE("mmio narrow stores at any lane write the word at its address") {
    Fixture  f;
    uint32_t expect = 0xBEBEBEBE;
    SUBCASE("byte at +1") {
        f.program({kStrbR1R2});
        f.cpu.set_reg(Reg::R2, kRegs + 9);
    }
    SUBCASE("byte at +3") {
        f.program({kStrbR1R2});
        f.cpu.set_reg(Reg::R2, kRegs + 11);
    }
    SUBCASE("halfword at +2") {
        f.program({kStrhR1R2});
        f.cpu.set_reg(Reg::R2, kRegs + 10);
        expect = 0xBABEBABE;
    }
    f.cpu.set_reg(Reg::R1, 0xCAFEBABE);
    CHECK(f.run() == kOk);
    CHECK(f.dev.calls == std::vector<Call>{
                             {"write", "regs", 8, 4, expect}
    });
}

TEST_CASE("unclaimed address reads 0 and ignores writes") {
    Fixture f;
    f.program({kLdrR1R2, kLdrR3R2, kStrR1R2, kStrR1R2, kLdrR3R4, kStrR1R4});
    f.cpu.set_reg(Reg::R1, 0x55);
    f.cpu.set_reg(Reg::R3, 0x66);
    f.cpu.set_reg(Reg::R2, 0x00800100);
    f.cpu.set_reg(Reg::R4, 0x00800104);
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::R1) == 0);
    CHECK(f.r(Reg::R3) == 0);
    CHECK(f.dev.calls.empty());
    f.start();
    f.cpu.set_reg(Reg::R2, 0x00800100);
    f.cpu.set_reg(Reg::R4, 0x00800104);
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::PC) == 0x118);
}

TEST_CASE("module range outside the peripheral window") {
    Fixture f;
    f.dev.value["mac"] = 0x11223344;
    f.program({kLdrR1R2, kLdrR3R4});
    f.cpu.set_reg(Reg::R2, kMac + 4);
    f.cpu.set_reg(Reg::R4, 0x00A00010); // same page, not claimed
    f.cpu.set_reg(Reg::R3, 0x66);
    CHECK(f.run() == kOk);
    CHECK(f.dev.calls == std::vector<Call>{
                             {"read", "mac", 4, 4, 0}
    });
    CHECK(f.r(Reg::R1) == 0x11223344);
    CHECK(f.r(Reg::R3) == 0);
}

TEST_CASE("fault from module reaches cpu") {
    Fixture f;
    f.dev.fault = true;
    f.program({kLdrR1R2});
    f.cpu.set_reg(Reg::R2, kRegs + 4);
    CHECK(f.run() == kOk);
    CHECK(f.dev.calls == std::vector<Call>{
                             {"read", "regs", 4, 4, 0}
    });
    CHECK(f.r(Reg::CPSR) == (kAbt | kI));
    CHECK(f.r(Reg::SPSR) == kSys);
    CHECK(f.r(Reg::LR) == 0x108);
    CHECK(f.r(Reg::PC) == Fixture::handler(0x10));
}

TEST_CASE("unmapped access aborts") {
    Fixture f;
    SUBCASE("data access") {
        f.program({kLdrR1R2});
        for (uint32_t addr : {0x00300000u, 0x00300010u, 0x00301FFCu}) {
            f.start();
            f.cpu.set_reg(Reg::R2, addr);
            CHECK(f.run() == kOk);
            CHECK(f.r(Reg::CPSR) == (kAbt | kI));
            CHECK(f.r(Reg::LR) == 0x108);
            CHECK(f.r(Reg::PC) == Fixture::handler(0x10));
        }
    }
    SUBCASE("write") {
        f.program({kStrR1R2});
        f.cpu.set_reg(Reg::R2, 0x00300008);
        CHECK(f.run() == kOk);
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == 0x108);
    }
    SUBCASE("fetch") {
        f.program({kBxR2});
        for (uint32_t addr : {0x00300000u, 0x00300040u}) {
            f.start();
            f.cpu.set_reg(Reg::R2, addr);
            CHECK(f.run() == kOk);
            CHECK(f.r(Reg::CPSR) == (kAbt | kI));
            CHECK(f.r(Reg::LR) == addr + 4);
            CHECK(f.r(Reg::PC) == Fixture::handler(0x0C));
        }
    }
}

TEST_CASE("fetch from non executable memory is pabt") {
    Fixture f;
    f.program({kBxR2});
    for (uint32_t addr : {kData, kData + 8, kData + 0x1000}) {
        f.start();
        f.cpu.set_reg(Reg::R2, addr);
        CHECK(f.run() == kOk);
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == addr + 4);
        CHECK(f.r(Reg::PC) == Fixture::handler(0x0C));
    }
    // Data accesses to it are fine.
    f.program({kStrR1R2, kLdrR3R2});
    f.cpu.set_reg(Reg::R1, 0x77);
    f.cpu.set_reg(Reg::R2, kData + 0x20);
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::R3) == 0x77);
}

TEST_CASE("overlap rejected") {
    Fixture f;
    auto    message = [](auto &&fn) {
        try {
            fn();
        } catch (const std::invalid_argument &e) {
            return std::string(e.what());
        }
        return std::string("no exception");
    };
    FakeDev dev2("dev2");
    CHECK(message([&] { f.bus.add_memory("ram2", kData + 0xFFFF, 0x1000, true); }) == "ram2 overlaps data");
    CHECK(message([&] { f.bus.add_memory("ram3", 0x00A00000, 0x1000, true); }) == "ram3 overlaps dev.mac");
    CHECK(message([&] {
              f.bus.claim(dev2,
                          {
                              {"regs",  kOther + 0xC},
                              {"other", 0x00810000  },
                              {"mac",   0x00A10000  }
              });
          }) == "dev2.regs overlaps dev.other");
    CHECK(message([&] {
              f.bus.claim(dev2,
                          {
                              {"regs",  0x00810000},
                              {"other", 0xFFF8    },
                              {"mac",   0x00A10000}
              });
          }) == "dev2.other overlaps itcm");
    CHECK(message([&] {
              f.bus.claim(dev2,
                          {
                              {"regs",  0x00810000   },
                              {"other", 0x00810020   },
                              {"mac",   kRom + 0x1FFF}
              });
          }) == "dev2.mac overlaps rom.code");
    CHECK(message([&] {
              f.bus.claim(dev2,
                          {
                              {"regs",  0x00810000},
                              {"other", 0x00810020}
              });
          }) == "dev2: no base for range mac");
    // A rejected claim leaves nothing behind.
    CHECK(f.bus.module_ranges().size() == 4);
    CHECK(static_cast<bool>(f.dev.invalidate));
    CHECK_FALSE(static_cast<bool>(dev2.invalidate));
}

TEST_CASE("adjacent ranges do not overlap") {
    Fixture f;
    FakeDev dev2("dev2");
    // Directly after dev.other, directly before the window end, after itcm.
    CHECK_NOTHROW(f.bus.claim(dev2,
                              {
                                  {"regs",  kOther + 0x10         },
                                  {"other", kWin + kWinSize - 0x10},
                                  {"mac",   0x00010000            }
    }));
    CHECK_NOTHROW(f.bus.add_memory("ram2", kData + 0x10000, 0x1000, true));
    dev2.value["regs"]   = 0x99;
    f.dev.value["other"] = 0x88;
    f.program({kLdrR1R2, kLdrR3R4});
    f.cpu.set_reg(Reg::R2, kOther + 0x10);
    f.cpu.set_reg(Reg::R4, kOther + 0xC);
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::R1) == 0x99);
    CHECK(f.r(Reg::R3) == 0x88);
    CHECK(dev2.calls == std::vector<Call>{
                            {"read", "regs", 0, 4, 0}
    });
    CHECK(f.dev.calls == std::vector<Call>{
                             {"read", "other", 0xC, 4, 0}
    });
}

TEST_CASE("memory regions and module ranges are listed") {
    Fixture f;
    auto    regions = f.bus.memory_regions();
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].name == "itcm");
    CHECK(regions[0].base == 0);
    CHECK(regions[0].size == 0x10000);
    CHECK(regions[0].executable);
    CHECK(regions[1].name == "data");
    CHECK(regions[1].base == kData);
    CHECK_FALSE(regions[1].executable);
    auto ranges = f.bus.module_ranges();
    REQUIRE(ranges.size() == 4);
    auto info = [](const RangeInfo &i) { return std::make_tuple(i.module, i.range, i.base, i.size); };
    CHECK(info(ranges[0]) == std::make_tuple(std::string("dev"), std::string("regs"), kRegs, 0x20u));
    CHECK(info(ranges[1]) == std::make_tuple(std::string("dev"), std::string("other"), kOther, 0x10u));
    CHECK(info(ranges[2]) == std::make_tuple(std::string("dev"), std::string("mac"), kMac, 0x100u));
    CHECK(info(ranges[3]) == std::make_tuple(std::string("rom"), std::string("code"), kRom, 0x2000u));

    const uint8_t bytes[] = {1, 2, 3, 4};
    f.bus.write_memory(kData + 0xFFFE, std::span<const uint8_t>(bytes, 2));
    CHECK(f.cpu.mem_read(kData + 0xFFFE, 2) == std::vector<uint8_t>{1, 2});
    CHECK_THROWS_AS(f.bus.write_memory(kData + 0xFFFE, std::span<const uint8_t>(bytes, 4)), std::invalid_argument);
}

TEST_CASE("demand page mapped on first read") {
    Fixture f;
    f.program({kLdrR1R2, kLdrR3R2, kLdrR3R4});
    f.cpu.set_reg(Reg::R2, kRom + 0x10);
    f.cpu.set_reg(Reg::R4, kRom + 0x1008);
    CHECK_FALSE(f.mapped(kRom));
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::R1) == f.rom.word(0x10));
    CHECK(f.r(Reg::R3) == f.rom.word(0x1008));
    CHECK(f.rom.pages == std::vector<uint32_t>{0, 0x1000});
    CHECK(f.rom.calls.empty());
    CHECK(f.cpu.mem_read(kRom + 0xFFC, 4) ==
          std::vector<uint8_t>(f.rom.mem.begin() + 0xFFC, f.rom.mem.begin() + 0x1000));

    // Code runs from a demand page too.
    f.rom.put(0x1800, mov_r0(7));
    f.rom.put(0x1804, b(kRom + 0x1804, kRom + 0x1804));
    f.rom.invalidate("code", 0x1800, 8);
    f.program({kBxR2});
    f.cpu.set_reg(Reg::R2, kRom + 0x1800);
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::R0) == 7);
    CHECK(f.r(Reg::PC) == kRom + 0x1804);
}

TEST_CASE("demand bad span aborts, pabt for fetch") {
    Fixture f;
    f.rom.bad[0] = {
        {0x100, 0x120}
    };
    SUBCASE("read") {
        f.program({kLdrR1R2});
        f.cpu.set_reg(Reg::R2, kRom + 0x11E);
        CHECK(f.run() == kOk);
        CHECK(f.rom.calls == std::vector<Call>{
                                 {"read", "code", 0x11E, 4, 0}
        });
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == 0x108);
        CHECK(f.r(Reg::PC) == Fixture::handler(0x10));
    }
    SUBCASE("fetch") {
        f.program({kBxR2});
        f.cpu.set_reg(Reg::R2, kRom + 0x104);
        CHECK(f.run() == kOk);
        REQUIRE(f.rom.calls.size() == 1);
        CHECK(std::get<1>(f.rom.calls[0]) == "code");
        CHECK(std::get<2>(f.rom.calls[0]) == 0x104);
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == kRom + 0x108);
        CHECK(f.r(Reg::PC) == Fixture::handler(0x0C));
    }
    // The page was not mapped.
    CHECK(f.rom.pages == std::vector<uint32_t>{0});
    CHECK_FALSE(f.mapped(kRom));
}

TEST_CASE("bus master reads and writes RAM across region ends only within regions") {
    Fixture       f;
    BusMemory    &m       = f.bus.memory();
    const uint8_t bytes[] = {1, 2, 3, 4, 5};
    m.write(kData + 0x10, bytes);
    CHECK(m.read(kData + 0x10, 5) == std::vector<uint8_t>{1, 2, 3, 4, 5});
    CHECK(f.cpu.mem_read(kData + 0x12, 1) == std::vector<uint8_t>{3});
    CHECK(m.read(kData, 0).empty());
    // Data written by a bus master is also visible to the CPU's code cache.
    const uint8_t last[] = {9, 8};
    m.write(kData + 0xFFFE, last);
    CHECK(m.read(kData + 0xFFFE, 2) == std::vector<uint8_t>{9, 8});
}

TEST_CASE("bus master access that leaves RAM is a bus error") {
    Fixture    f;
    BusMemory &m = f.bus.memory();
    // The last two bytes of a region are fine, a third one is not.
    CHECK_THROWS_WITH_AS(m.read(kData + 0xFFFE, 4), "bus error at 0x00410000: nothing readable there", BusError);
    const uint8_t bytes[] = {1, 2, 3, 4};
    CHECK_THROWS_WITH_AS(m.write(kData + 0xFFFE, bytes), "bus error at 0x00410000: nothing writable there", BusError);
    // The part before the error was written.
    CHECK(m.read(kData + 0xFFFE, 2) == std::vector<uint8_t>{1, 2});
    // Peripheral registers, unmapped space and demand ranges (write) are not reachable.
    CHECK_THROWS_AS(m.read(kRegs, 4), BusError);
    CHECK_THROWS_AS(m.write(kRegs, bytes), BusError);
    CHECK_THROWS_AS(m.read(0x30000000, 4), BusError);
    CHECK_THROWS_WITH_AS(m.write(kRom, bytes), "bus error at 0x00100000: nothing writable there", BusError);
    try {
        m.read(0x30000000, 4);
    } catch (const BusError &e) {
        CHECK(e.addr == 0x30000000);
    }
}

namespace {

// RAM at 0x00400000, 64 KiB repeated over 256 KiB, code at 0.
struct MirrorFixture {
    std::ostringstream out;
    Cpu                cpu;
    Bus                bus{cpu};

    static constexpr uint32_t kBase = 0x00400000, kSize = 0x10000, kWindow = 0x40000;

    MirrorFixture() {
        Logger::reset_for_tests();
        Logger::configure(out, false);
        bus.add_memory("itcm", 0, 0x10000, true);
        bus.add_memory("ram", kBase, kSize, false, kWindow);
    }
    ~MirrorFixture() { Logger::reset_for_tests(); }

    void put(uint32_t addr, uint32_t word) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(word),
                                  static_cast<uint8_t>(word >> 8),
                                  static_cast<uint8_t>(word >> 16),
                                  static_cast<uint8_t>(word >> 24)};
        cpu.mem_write(addr, bytes);
    }
    uint32_t word(uint32_t addr) {
        std::vector<uint8_t> b = cpu.mem_read(addr, 4);
        return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24;
    }
    // Runs `code` from 0x100 with r2 = `r2`, r4 = `r4`, r1 = `r1`.
    void run(std::vector<uint32_t> code, uint32_t r1, uint32_t r2, uint32_t r4 = 0) {
        uint32_t a = 0x100;
        for (uint32_t w : code) put(a, w), a += 4;
        put(a, b(a, a));
        REQUIRE(uc_ctl_flush_tb(cpu.engine()) == UC_ERR_OK);
        cpu.reset(0x100);
        cpu.set_reg(Reg::CPSR, kSys);
        cpu.set_reg(Reg::R1, r1);
        cpu.set_reg(Reg::R2, r2);
        cpu.set_reg(Reg::R4, r4);
        cpu.run(static_cast<uint32_t>(code.size()) + 1);
    }
};

} // namespace

TEST_CASE("a mirror reaches the same bytes") {
    using F = MirrorFixture;
    F f;
    // A store through the third copy lands in the region.
    f.run({kStrR1R2}, 0x11223344, F::kBase + 2 * F::kSize + 0x40);
    CHECK(f.word(F::kBase + 0x40) == 0x11223344);
    // A load through the last copy.
    f.put(F::kBase + 0x80, 0xCAFEF00D);
    f.run({kLdrR3R2, kLdrR3R2}, 0, F::kBase + 3 * F::kSize + 0x80);
    CHECK(f.cpu.reg(Reg::R3) == 0xCAFEF00D);
    // A byte store changes that byte only.
    f.run({kStrbR1R2}, 0xA5, F::kBase + F::kSize + 0x81);
    CHECK(f.word(F::kBase + 0x80) == 0xCAFEA50D);
}

TEST_CASE("a bus master reaches the region through a mirror") {
    using F = MirrorFixture;
    F             f;
    BusMemory    &m       = f.bus.memory();
    const uint8_t bytes[] = {1, 2, 3, 4};
    m.write(F::kBase + F::kSize + 0x10, bytes);
    CHECK(f.cpu.mem_read(F::kBase + 0x10, 4) == std::vector<uint8_t>{1, 2, 3, 4});
    CHECK(m.read(F::kBase + 3 * F::kSize + 0x10, 4) == std::vector<uint8_t>{1, 2, 3, 4});
    CHECK(m.read(F::kBase + 3 * F::kSize + 0x20, 4).size() == 4);
    // A transfer across the end of a copy goes on at the start of the next.
    f.put(F::kBase + F::kSize - 4, 0xBBAA0000);
    f.put(F::kBase, 0x0000DDCC);
    CHECK(m.read(F::kBase + 2 * F::kSize - 2, 4) == std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD});
    CHECK(f.bus.memory_regions()[1].span == F::kWindow);
}

TEST_CASE("a mirrored region must be aligned and its window must fit") {
    Cpu cpu;
    Bus bus{cpu};
    CHECK_THROWS_WITH_AS(bus.add_memory("a", 0x400000, 0x10000, false, 0x18000),
                         "a: the window must be a multiple of the size",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(bus.add_memory("b", 0x400800, 0x10000, false, 0x20000),
                         "b: a region with mirrors must be 4 KiB aligned",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        bus.add_memory("c", 0xFFFF0000, 0x10000, false, 0x20000), "c passes the end of memory", std::invalid_argument);
    bus.add_memory("ram", 0x400000, 0x10000, false, 0x40000);
    CHECK_THROWS_WITH_AS(bus.add_memory("late", 0x430000, 0x1000, false), "late overlaps ram", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        bus.claim_unclaimed(0x420000, 0x10000), "peripheral window overlaps ram", std::invalid_argument);
}

TEST_CASE("bus master reads a demand range page by page") {
    Fixture    f;
    BusMemory &m = f.bus.memory();
    auto       v = m.read(kRom + 0xFFC, 8); // crosses from page 0 to page 1
    REQUIRE(v.size() == 8);
    for (uint32_t i = 0; i < 8; ++i) CHECK(v[i] == f.rom.mem[0xFFC + i]);
    CHECK(f.rom.pages == std::vector<uint32_t>{0, 0x1000});
}

TEST_CASE("bus master read of a bad span is a bus error after the module reported it") {
    Fixture f;
    f.rom.bad[0x1000] = {
        {0x10, 0x20}
    };
    BusMemory &m = f.bus.memory();
    CHECK_NOTHROW(m.read(kRom + 0x1000, 0x10)); // up to the span
    f.rom.calls.clear();
    CHECK_THROWS_WITH_AS(m.read(kRom + 0x1008, 0x10), "bus error at 0x00101008: unreadable block", BusError);
    REQUIRE(f.rom.calls.size() == 1);
    CHECK(std::get<2>(f.rom.calls[0]) == 0x1008);
    // A read that starts on the previous page and runs into the span fails too.
    CHECK_THROWS_AS(m.read(kRom + 0xFF0, 0x30), BusError);
}

TEST_CASE("bad span adds hooks and requests restart") {
    Fixture f;
    f.rom.bad[0] = {
        {0x40, 0x60}
    };
    // Page code: mov r0, #5; ldr r1, [r4]; b 0x40 (into the bad span).
    f.rom.put(0x0, mov_r0(5));
    f.rom.put(0x4, kLdrR1R4);
    f.rom.put(0x8, b(kRom + 0x8, kRom + 0x40));
    f.program({kBxR2});
    f.cpu.set_reg(Reg::R2, kRom);
    SUBCASE("instruction in a bad span is a prefetch abort") {
        f.cpu.set_reg(Reg::R4, kRom + 0x20);
        CHECK(f.run() == kOk);
        CHECK(f.r(Reg::R0) == 5);
        CHECK(f.r(Reg::R1) == f.rom.word(0x20));
        REQUIRE(f.rom.calls.size() == 1);
        CHECK(std::get<2>(f.rom.calls[0]) == 0x40);
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == kRom + 0x44);
        CHECK(f.r(Reg::PC) == Fixture::handler(0x0C));
    }
    SUBCASE("read of a bad span is a data abort") {
        f.cpu.set_reg(Reg::R4, kRom + 0x5E);
        CHECK(f.run() == kOk);
        CHECK(f.rom.calls == std::vector<Call>{
                                 {"read", "code", 0x5E, 4, 0}
        });
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == kRom + 0xC);
        CHECK(f.r(Reg::PC) == Fixture::handler(0x10));
    }
    SUBCASE("a bad read in the block that mapped the page aborts") {
        // Both loads sit in one block translated before the page had hooks.
        f.program({kLdrR1R2, kLdrR3R4});
        f.cpu.set_reg(Reg::R2, kRom + 0x20);
        f.cpu.set_reg(Reg::R4, kRom + 0x50);
        CHECK(f.run() == kOk);
        CHECK(f.r(Reg::R1) == f.rom.word(0x20));
        CHECK(f.rom.calls == std::vector<Call>{
                                 {"read", "code", 0x50, 4, 0}
        });
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        CHECK(f.r(Reg::LR) == 0x10C);
        CHECK(f.r(Reg::PC) == Fixture::handler(0x10));
    }
    SUBCASE("invalidate removes the hooks") {
        f.cpu.set_reg(Reg::R4, kRom + 0x40);
        CHECK(f.run(3) == Result{RunResult::Budget, 3}); // bx, mov, ldr aborts
        CHECK(f.r(Reg::CPSR) == (kAbt | kI));
        f.rom.bad.clear();
        f.rom.put(0x40, b(kRom + 0x40, kRom + 0x40));
        f.rom.invalidate("code", 0x44, 4);
        f.rom.calls.clear();
        f.program({kBxR2});
        f.cpu.set_reg(Reg::R2, kRom);
        f.cpu.set_reg(Reg::R4, kRom + 0x40);
        CHECK(f.run() == kOk);
        CHECK(f.rom.calls.empty());
        CHECK(f.r(Reg::CPSR) == kSys);
        CHECK(f.r(Reg::R1) == f.rom.word(0x40));
        CHECK(f.r(Reg::PC) == kRom + 0x40);
    }
    CHECK(f.rom.pages.front() == 0);
}

TEST_CASE("write to demand page ignored") {
    Fixture f;
    f.program({kStrR1R2, kStrbR1R2, kStrR1R4, kLdrR3R4});
    f.cpu.set_reg(Reg::R1, 0xDEADBEEF);
    f.cpu.set_reg(Reg::R2, kRom + 0x1000); // page not mapped yet
    f.cpu.set_reg(Reg::R4, kRom + 0x8);
    CHECK(f.run() == kOk);
    CHECK(f.r(Reg::CPSR) == kSys);
    CHECK(f.r(Reg::PC) == 0x110);
    CHECK(f.r(Reg::R3) == f.rom.word(0x8));
    CHECK(f.cpu.mem_read(kRom + 0x1000, 4) ==
          std::vector<uint8_t>(f.rom.mem.begin() + 0x1000, f.rom.mem.begin() + 0x1004));
    CHECK(f.cpu.mem_read(kRom + 0x8, 4) == std::vector<uint8_t>(f.rom.mem.begin() + 0x8, f.rom.mem.begin() + 0xC));
}

TEST_CASE("invalidate unmaps pages") {
    Fixture f;
    f.program({kLdrR1R2, kLdrR3R4});
    auto run_reads = [&] {
        f.start();
        f.cpu.set_reg(Reg::R2, kRom + 0x10);
        f.cpu.set_reg(Reg::R4, kRom + 0x1010);
        return f.run();
    };
    CHECK(run_reads() == kOk);
    CHECK(f.rom.pages == std::vector<uint32_t>{0, 0x1000});

    const uint32_t old = f.rom.word(0x1010);
    f.rom.put(0x10, 0x01020304);
    f.rom.put(0x1010, 0x05060708);
    SUBCASE("only the touched page") {
        f.rom.invalidate("code", 0x10, 4);
        CHECK_FALSE(f.mapped(kRom));
        CHECK(f.mapped(kRom + 0x1000));
        CHECK(run_reads() == kOk);
        CHECK(f.r(Reg::R1) == 0x01020304);
        CHECK(f.r(Reg::R3) == old); // still the cached page
        CHECK(f.rom.pages == std::vector<uint32_t>{0, 0x1000, 0});
    }
    SUBCASE("a span crossing a page boundary") {
        f.rom.invalidate("code", 0xFFF, 2);
        CHECK_FALSE(f.mapped(kRom));
        CHECK_FALSE(f.mapped(kRom + 0x1000));
        CHECK(run_reads() == kOk);
        CHECK(f.r(Reg::R1) == 0x01020304);
        CHECK(f.r(Reg::R3) == 0x05060708);
        CHECK(f.rom.pages == std::vector<uint32_t>{0, 0x1000, 0, 0x1000});
    }
}

TEST_CASE("invalidate shows new flash data") {
    Cpu   cpu;
    Bus   bus(cpu);
    Flash flash("flash", 0x10000);
    bus.add_memory("itcm", 0, 0x10000, true);
    bus.claim(flash,
              {
                  {"xip",  0x00200000},
                  {"ctrl", 0x00802000}
    });
    bus.claim_unclaimed(kWin, kWinSize);

    auto block = [](uint8_t first) {
        std::vector<uint8_t> blk;
        for (int i = 0; i < 32; ++i) blk.push_back(static_cast<uint8_t>(first + i));
        uint16_t crc = crc16(blk);
        blk.push_back(static_cast<uint8_t>(crc >> 8));
        blk.push_back(static_cast<uint8_t>(crc));
        return blk;
    };
    const uint32_t code[] = {kLdrR1R2, b(0x104, 0x104)};
    cpu.mem_write(0x100, std::span(reinterpret_cast<const uint8_t *>(code), sizeof code));
    auto read_xip = [&] {
        cpu.reset(0x100);
        cpu.set_reg(Reg::R2, 0x00200004);
        CHECK(cpu.run(20) == Result{RunResult::Budget, 20});
        return cpu.reg(Reg::R1);
    };

    flash.load(0, block(0x10));
    CHECK(read_xip() == 0x17161514);
    flash.load(0, block(0x40));
    CHECK(read_xip() == 0x47464544);
}
