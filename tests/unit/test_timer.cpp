#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include "periph/timer.h"

using namespace bk;

namespace {

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct Fixture {
    FakeClock         clock;
    std::vector<bool> calls;
    Timer             timer;

    explicit Fixture(uint32_t hz = 26000000) : timer("timer", clock, [this](bool v) { calls.push_back(v); }, hz) {}

    void     w(uint32_t off, uint32_t v) { timer.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return timer.read("regs", off, 4); }
};

} // namespace

TEST_CASE("counter 0 does not run") {
    Fixture f;
    f.w(0x0C, 1);
    CHECK_FALSE(f.timer.next_event_ns().has_value());
    CHECK(f.timer.ranges().at(0).name == "regs");
    CHECK(f.timer.ranges().at(0).size == 0x10);
    CHECK(f.timer.name() == "timer");
}

TEST_CASE("period uses divider") {
    Fixture f;
    f.clock.now = 500;
    f.w(0x00, 26);
    f.w(0x0C, 1);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(1500));
    CHECK(f.r(0x00) == 26);
    CHECK(f.r(0x0C) == 1);

    Fixture g;
    g.w(0x04, 26);
    g.w(0x0C, 2 | (3u << 3)); // divider 3 -> period 4000
    CHECK(g.timer.next_event_ns() == std::optional<uint64_t>(4000));
    CHECK(g.r(0x0C) == (2u | (3u << 3)));
}

TEST_CASE("tick sets status and line, write 1 clears") {
    Fixture f;
    f.w(0x00, 26);
    f.w(0x0C, 1);
    CHECK(f.calls.back() == false);
    f.timer.tick(999);
    CHECK(f.r(0x0C) == 1);
    CHECK(f.calls.back() == false);
    f.timer.tick(1000);
    CHECK(f.r(0x0C) == (1u | (1u << 7)));
    CHECK(f.calls.back() == true);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(2000));
    f.w(0x0C, 1 | (1u << 7));
    CHECK(f.r(0x0C) == 1);
    CHECK(f.calls.back() == false);
    CHECK(f.calls == std::vector<bool>{false, false, true, false});
    // Status of timer 1 sits at bit 8; clearing bit 7 leaves it.
    f.w(0x04, 26);
    f.w(0x0C, 3);
    f.timer.tick(2000);
    CHECK(f.r(0x0C) == (3u | (3u << 7)));
    f.w(0x0C, 3 | (1u << 7));
    CHECK(f.r(0x0C) == (3u | (1u << 8)));
    CHECK(f.calls.back() == true);
}

TEST_CASE("missed periods skipped") {
    Fixture f;
    f.w(0x00, 26);
    f.w(0x0C, 1);
    f.timer.tick(5500);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(6000));
    CHECK(f.r(0x0C) == (1u | (1u << 7)));
}

TEST_CASE("running timer keeps next, stopped timer drops it") {
    Fixture f;
    f.w(0x00, 26);
    f.w(0x0C, 1);
    f.clock.now = 300;
    f.w(0x0C, 1 | (2u << 3));
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(1000));
    f.w(0x0C, 0);
    CHECK_FALSE(f.timer.next_event_ns().has_value());
    f.w(0x0C, 1);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(300 + 1000 * 1));
}

TEST_CASE("earliest next of several timers") {
    Fixture f;
    f.w(0x00, 52);
    f.w(0x08, 13);
    f.w(0x0C, 5);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(500));
}

TEST_CASE("divider 15 no overflow") {
    Fixture f(32000);
    f.clock.now = 1000;
    f.w(0x00, 0xFFFFFFFF);
    f.w(0x0C, 1 | (15u << 3));
    uint64_t expect =
        1000 + static_cast<uint64_t>(static_cast<unsigned __int128>(0xFFFFFFFFULL) * 16 * 1'000'000'000ULL / 32000);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(expect));
    CHECK(f.r(0x0C) == (1u | (15u << 3)));

    // hz 1: period exceeds 64 bits; next must not wrap below now.
    Fixture g(1);
    g.clock.now = 0xFFFFFFFFFFFFFF00ULL;
    g.w(0x00, 0xFFFFFFFF);
    g.w(0x0C, 1 | (15u << 3));
    REQUIRE(g.timer.next_event_ns().has_value());
    CHECK(*g.timer.next_event_ns() >= g.clock.now);
    g.timer.tick(*g.timer.next_event_ns());
    CHECK(*g.timer.next_event_ns() >= g.clock.now);

    Fixture h(1);
    h.clock.now = 10;
    h.w(0x00, 0xFFFFFFFF);
    h.w(0x0C, 1 | (15u << 3));
    CHECK(h.timer.next_event_ns() == std::optional<uint64_t>(10 + (1ULL << 62)));
}

TEST_CASE("unknown offsets read 0 and ignore writes") {
    Fixture f;
    f.w(0x10, 5);
    f.w(0x20, 5);
    CHECK(f.r(0x10) == 0);
    CHECK(f.r(0x14) == 0);
    CHECK(f.r(0x0C) == 0);
}

TEST_CASE("reset clears") {
    Fixture f;
    f.w(0x00, 26);
    f.w(0x08, 7);
    f.w(0x0C, 5 | (4u << 3));
    f.timer.tick(10'000'000);
    CHECK(f.calls.back() == true);
    f.timer.reset();
    CHECK(f.r(0x00) == 0);
    CHECK(f.r(0x08) == 0);
    CHECK(f.r(0x0C) == 0);
    CHECK_FALSE(f.timer.next_event_ns().has_value());
    CHECK(f.calls.back() == false);
    CHECK(f.calls == std::vector<bool>{false, true, false});
}

TEST_CASE("counter write does not schedule") {
    Fixture f;
    f.clock.now = 100;
    f.w(0x0C, 1);
    CHECK_FALSE(f.timer.next_event_ns().has_value());
    f.w(0x00, 26);
    CHECK_FALSE(f.timer.next_event_ns().has_value());
    f.timer.tick(10'000);
    CHECK(f.r(0x0C) == 1);
    f.clock.now = 200;
    f.w(0x0C, 1);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(1200));
}

TEST_CASE("running timer with counter written to 0 keeps firing") {
    Fixture f;
    f.w(0x00, 26);
    f.w(0x0C, 1);
    f.w(0x00, 0);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(1000));
    f.timer.tick(1000);
    CHECK(f.r(0x0C) == (1u | (1u << 7)));
    // period is now max(0,1) * 1e9 / 26e6 = 38 ns
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(1038));
}

TEST_CASE("tick with status already set advances next") {
    Fixture f;
    f.w(0x00, 26);
    f.w(0x0C, 1);
    f.timer.tick(1000);
    f.timer.tick(2000);
    CHECK(f.r(0x0C) == (1u | (1u << 7)));
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(3000));
    CHECK(f.calls == std::vector<bool>{false, true, true});
}

TEST_CASE("hz 0 is treated as 1") {
    Fixture f(0);
    f.w(0x00, 1);
    f.w(0x0C, 1);
    CHECK(f.timer.next_event_ns() == std::optional<uint64_t>(1'000'000'000));
}
