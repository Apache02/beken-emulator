#include <doctest/doctest.h>

#include <optional>
#include <sstream>

#include "core/log.h"
#include "periph/wdt.h"

using namespace bk;

namespace {

constexpr uint64_t kMs = 1'000'000;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct Fixture {
    FakeClock          clock;
    int                resets = 0;
    bool               off    = false;
    std::ostringstream out;
    Wdt                wdt;

    Fixture() : wdt("wdt", clock, [this] { ++resets; }, [this] { return off; }) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    void     w(uint32_t v) { wdt.write("regs", 0, 4, v); }
    uint32_t r(uint32_t off = 0) { return wdt.read("regs", off, 4); }
};

} // namespace

TEST_CASE("default period 0x4000 ms") {
    Fixture f;
    CHECK(f.r() == 0x4000);
    CHECK(f.wdt.name() == "wdt");
    CHECK(f.wdt.ranges().at(0).name == "regs");
    CHECK(f.wdt.ranges().at(0).size == 4);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
    f.w(0xFF123456);
    CHECK(f.r() == 0x123456);
    CHECK(f.r(4) == 0);
    f.w(0x1234);
    f.wdt.write("regs", 4, 4, 0x55);
    CHECK(f.r() == 0x1234);
}

TEST_CASE("expires and resets chip") {
    Fixture f;
    f.w(10);
    f.wdt.tick(0);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(10 * kMs));
    f.wdt.tick(10 * kMs - 1);
    CHECK(f.resets == 0);
    f.wdt.tick(11 * kMs);
    CHECK(f.resets == 1);
    CHECK(f.out.str().find("watchdog expired: resetting the chip") != std::string::npos);
    CHECK(f.out.str().find("WARNING") != std::string::npos);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
}

TEST_CASE("kick 0x5A then 0xA5 restarts") {
    Fixture f;
    f.w(10);
    f.wdt.tick(0);
    f.clock.now = 6 * kMs;
    f.w(0x5A0000 | 10);
    f.w(0xA50000 | 10);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(16 * kMs));
    f.wdt.tick(15 * kMs);
    CHECK(f.resets == 0);
    f.wdt.tick(16 * kMs);
    CHECK(f.resets == 1);
    CHECK(f.r() == (0xA50000u | 10));
}

TEST_CASE("kick uses the period written with the key") {
    Fixture f;
    f.w(10);
    f.wdt.tick(0);
    f.clock.now = 2 * kMs;
    f.w(0x5A0000 | 20);
    f.w(0xA50000 | 20);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(22 * kMs));
}

TEST_CASE("wrong key sequence is not a kick") {
    Fixture f;
    f.w(10);
    f.wdt.tick(0);
    f.clock.now = 5 * kMs;
    f.w(0x5A0000 | 10);
    f.w(0x5A0000 | 10);
    f.w(0xA50000 | 10);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(15 * kMs));

    f.clock.now = 8 * kMs;
    f.w(0x5A0000 | 10);
    f.w(0x110000 | 10);
    f.w(0xA50000 | 10);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(15 * kMs));

    // A kick clears the first-key flag: a lone 0xA5 after it is not a kick.
    f.w(0xA50000 | 10);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(15 * kMs));
}

TEST_CASE("clock_off pauses") {
    Fixture f;
    f.w(10);
    f.wdt.tick(0);
    f.off = true;
    f.wdt.tick(50 * kMs);
    CHECK(f.resets == 0);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
    f.off = false;
    f.wdt.tick(60 * kMs);
    CHECK(f.resets == 0);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(70 * kMs));
    f.wdt.tick(70 * kMs);
    CHECK(f.resets == 1);

    // A kick while stopped drops the countdown.
    f.wdt.tick(71 * kMs);
    f.off = true;
    f.w(0x5A0000 | 10);
    f.w(0xA50000 | 10);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
}

TEST_CASE("next_event_ns is deadline") {
    Fixture f;
    f.clock.now = 3 * kMs;
    f.w(10);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
    f.wdt.tick(3 * kMs);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(13 * kMs));
    // The current register period applies to a running countdown.
    f.w(20);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(23 * kMs));
    f.wdt.reset();
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
    CHECK(f.r() == 0x4000);
    f.wdt.tick(100 * kMs);
    CHECK(f.wdt.next_event_ns() == std::optional<uint64_t>(100 * kMs + 0x4000 * kMs));
}

TEST_CASE("period 0 expires at once without loop") {
    Fixture f;
    f.w(0);
    f.wdt.tick(5);
    CHECK(f.resets == 1);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
    // The countdown restarts on the following tick, one reset per tick at most.
    f.wdt.tick(6);
    CHECK(f.resets == 2);
    CHECK_FALSE(f.wdt.next_event_ns().has_value());
}
