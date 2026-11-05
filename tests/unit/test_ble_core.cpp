#include <doctest/doctest.h>

#include <cstdint>

#include "periph/ble_core.h"

using namespace bk;

namespace {

constexpr uint32_t kRwblecntl = 0x00, kBasetimecnt = 0x1C, kFinetimecnt = 0x20, kBdaddrl = 0x24;
constexpr uint32_t kSamp = 0x80000000;
constexpr uint64_t kUs   = 1000;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct Fixture {
    FakeClock clock;
    BleCore   ble{"BLE", clock};

    uint32_t r(uint32_t off) { return ble.read("regs", off, 4); }
    void     w(uint32_t off, uint32_t v) { ble.write("regs", off, 4, v); }
};

} // namespace

TEST_CASE("ble core has a register block and an exchange memory") {
    Fixture f;
    CHECK(f.ble.name() == "BLE");
    REQUIRE(f.ble.ranges().size() == 2);
    CHECK(f.ble.ranges()[0].name == "regs");
    CHECK(f.ble.ranges()[0].size == 0x200);
    CHECK(f.ble.ranges()[1].name == "em");
    CHECK(f.ble.ranges()[1].size == 0x2000);
    CHECK_FALSE(f.ble.byte_lanes("regs"));
    CHECK(f.ble.byte_lanes("em"));
    BleCore small("BLE", f.clock, 0x1000);
    CHECK(small.ranges()[1].size == 0x1000);
}

TEST_CASE("SAMP samples the base and fine time counters and reads back cleared") {
    Fixture f;
    CHECK(f.r(kBasetimecnt) == 0);
    CHECK(f.r(kFinetimecnt) == 0);
    // 3 slots of 625 us and 100 us into the fourth: the fine counter counts down from 624.
    f.clock.now = (3 * 625 + 100) * kUs;
    f.w(kBasetimecnt, kSamp);
    CHECK(f.r(kBasetimecnt) == 3);
    CHECK(f.r(kFinetimecnt) == 524);
    // The counters hold the sample until the next one.
    f.clock.now = (10 * 625 + 624) * kUs;
    CHECK(f.r(kBasetimecnt) == 3);
    CHECK(f.r(kFinetimecnt) == 524);
    f.w(kBasetimecnt, kSamp);
    CHECK(f.r(kBasetimecnt) == 10);
    CHECK(f.r(kFinetimecnt) == 0);
    // A write without SAMP samples nothing; the base counter is 27 bits wide.
    f.clock.now = (uint64_t{1} << 27) * 625 * kUs + 5 * 625 * kUs;
    f.w(kBasetimecnt, 0);
    CHECK(f.r(kBasetimecnt) == 10);
    f.w(kBasetimecnt, kSamp);
    CHECK(f.r(kBasetimecnt) == 5);
}

TEST_CASE("the soft reset and software interrupt bits of RWBLECNTL complete at once") {
    Fixture f;
    f.w(kRwblecntl, 0xF0000100);
    CHECK(f.r(kRwblecntl) == 0x00000100);
}

TEST_CASE("other registers read back what was written, reset clears them") {
    Fixture f;
    f.w(kBdaddrl, 0x12345678);
    CHECK(f.r(kBdaddrl) == 0x12345678);
    CHECK(f.r(0x1FC) == 0);
    f.w(0x200, 1); // past the block: dropped
    f.ble.reset();
    CHECK(f.r(kBdaddrl) == 0);
}

TEST_CASE("the exchange memory is RAM written byte by byte") {
    Fixture f;
    f.ble.write("em", 0x10, 2, 0xBEEF);
    f.ble.write("em", 0x12, 2, 0x1234);
    CHECK(f.ble.read("em", 0x10, 4) == 0x1234BEEF);
    f.ble.write("em", 0x11, 1, 0x00);
    CHECK(f.ble.read("em", 0x10, 4) == 0x123400EF);
    f.ble.write("em", 0x1FFC, 4, 0xCAFEF00D);
    CHECK(f.ble.read("em", 0x1FFC, 4) == 0xCAFEF00D);
    f.ble.write("em", 0x2000, 4, 1); // past the end: dropped
    CHECK(f.ble.read("em", 0x2000, 4) == 0);
    f.ble.reset();
    CHECK(f.ble.read("em", 0x10, 4) == 0);
}
