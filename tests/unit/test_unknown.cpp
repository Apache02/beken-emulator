#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "core/log.h"
#include "periph/unknown.h"

using namespace bk;

namespace {

struct Fixture {
    std::ostringstream         out;
    std::optional<Instruction> insn;
    Unknown                    block;

    explicit Fixture(uint32_t size = 0x100, std::map<uint32_t, uint32_t> words = {}, bool with_source = true)
        : block("BLK",
                size,
                std::move(words),
                with_source ? Unknown::InstructionSource([this] { return insn; }) : nullptr) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    uint32_t r(uint32_t off, uint32_t size = 4) { return block.read("regs", off, size); }
    void     w(uint32_t off, uint32_t v, uint32_t size = 4) { block.write("regs", off, size, v); }
    // The last log line without its prefix and newline.
    std::string last() {
        std::string s = out.str();
        if (!s.empty() && s.back() == '\n') s.pop_back();
        auto nl = s.rfind('\n');
        return nl == std::string::npos ? s : s.substr(nl + 1);
    }
};

} // namespace

TEST_CASE("unknown has one range of the configured size") {
    Fixture f(192);
    REQUIRE(f.block.ranges().size() == 1);
    CHECK(f.block.ranges()[0].name == "regs");
    CHECK(f.block.ranges()[0].size == 192);
}

TEST_CASE("unknown holds 32-bit registers with start words") {
    Fixture f(0x40,
              {
                  {0x38, 0x0000f000},
                  {0x3C, 0x00001836}
    });
    CHECK(f.r(0x38) == 0xf000);
    CHECK(f.r(0x3C) == 0x1836);
    CHECK(f.r(0x00) == 0);
    f.w(0x10, 0xAABBCCDD);
    CHECK(f.r(0x10) == 0xAABBCCDD);
}

TEST_CASE("unknown reads 0 past the end of the block") {
    Fixture f(0x40,
              {
                  {0x3C, 0xAABBCCDD}
    });
    CHECK(f.r(0x3C) == 0xAABBCCDD);
    CHECK(f.r(0x40) == 0);
    CHECK(f.r(0x1000) == 0);
}

TEST_CASE("unknown narrow writes change only their bytes of the word") {
    Fixture f;
    f.w(0x10, 0x11223344);
    f.w(0x10, 0xAABBCCDD, 1);
    CHECK(f.r(0x10) == 0x112233DD);
    f.w(0x11, 0xEE, 1);
    CHECK(f.r(0x10) == 0x1122EEDD);
    f.w(0x12, 0x5566, 2);
    CHECK(f.r(0x10) == 0x5566EEDD);
    f.w(0x10, 0x1234, 2);
    CHECK(f.last().find("write +0x0010 (2 bytes) = 0x1234") != std::string::npos);
    CHECK(f.r(0x10) == 0x55661234);
    f.w(0x30, 0x1234, 1);
    CHECK(f.last().find("write +0x0030 (1 bytes) = 0x34") != std::string::npos);
}

TEST_CASE("unknown writes outside the block or across a word are dropped") {
    Fixture f(0x10);
    f.w(0x10, 0xFFFFFFFF);
    f.w(0x100, 1);
    f.w(0x0E, 0xAABBCCDD, 4); // a word write that is not aligned
    f.w(0x0F, 0xAABB, 2);     // a halfword across two words
    for (uint32_t off = 0; off < 0x10; off += 4) CHECK(f.r(off) == 0);
}

TEST_CASE("unknown size is rounded up to whole words") {
    Fixture f(6);
    f.w(4, 0xAABBCCDD);
    CHECK(f.r(4) == 0xAABBCCDD);
    CHECK(f.block.ranges()[0].size == 6);
}

TEST_CASE("unknown start words must be words inside the block") {
    for (uint32_t offset : {1u, 2u, 0x40u, 0x1000u}) {
        CHECK_THROWS_AS(Unknown("BLK",
                                0x40,
                                {
                                    {offset, 1}
        },
                                nullptr),
                        std::invalid_argument);
    }
    CHECK_NOTHROW(Unknown("BLK",
                          0x40,
                          {
                              {0x3C, 1}
    },
                          nullptr));
}

TEST_CASE("unknown logs every access at INFO with pc and opcode") {
    Fixture f;
    f.insn = Instruction{0x00001234, 0xE5801000, 4};
    f.w(0x10, 0x55);
    CHECK(f.last().ends_with("BLK write +0x0010 (4 bytes) = 0x00000055  pc 0x00001234 insn E5801000"));
    CHECK(f.last().find("INFO") != std::string::npos);
    CHECK(f.last().find("emulator.unknown") != std::string::npos);
    f.r(0x10);
    CHECK(f.last().ends_with("BLK read +0x0010 (4 bytes) = 0x00000055  pc 0x00001234 insn E5801000"));
    // Thumb opcodes have four digits.
    f.insn = Instruction{0x00002000, 0x6008, 2};
    f.r(0x14);
    CHECK(f.last().ends_with("BLK read +0x0014 (4 bytes) = 0x00000000  pc 0x00002000 insn 6008"));
}

TEST_CASE("unknown leaves the pc part out without an instruction") {
    Fixture none(0x100, {}, false);
    none.r(0x20);
    CHECK(none.last().ends_with("BLK read +0x0020 (4 bytes) = 0x00000000"));
    Fixture unreadable;
    unreadable.insn = std::nullopt;
    unreadable.r(0x20);
    CHECK(unreadable.last().ends_with("BLK read +0x0020 (4 bytes) = 0x00000000"));
}

TEST_CASE("unknown reset restores the start values") {
    Fixture f(0x40,
              {
                  {0x08, 7}
    });
    f.w(0x08, 99);
    f.w(0x10, 5);
    f.block.reset();
    CHECK(f.r(0x08) == 7);
    CHECK(f.r(0x10) == 0);
}
