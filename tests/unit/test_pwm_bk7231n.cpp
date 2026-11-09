#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include "periph/pwm_bk7231n.h"

using namespace bk;

namespace {

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct Fixture {
    FakeClock         clock;
    std::vector<bool> calls;
    PwmBk7231n        pwm;

    explicit Fixture(uint32_t hz = 26000000) : pwm("pwm", clock, [this](bool v) { calls.push_back(v); }, hz) {}

    void     w(uint32_t off, uint32_t v) { pwm.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return pwm.read("regs", off, 4); }
    void     advance(uint64_t ns) {
        clock.now += ns;
        pwm.tick(clock.now);
    }
};

// Control bits of the channel `post` of a group: mode, enable and interrupt.
constexpr uint32_t on(unsigned post, uint32_t mode = 1, bool irq = true) {
    return (mode | 1u << 3 | (irq ? 1u << 4 : 0u)) << (8 * post);
}

constexpr uint32_t ctrl(unsigned group) { return group * 0x40; }
// T1 to T4 (t = 1 to 4) of the channel `post` of a group.
constexpr uint32_t time_reg(unsigned group, unsigned post, unsigned t) {
    return ctrl(group) + 0x04 + 0x10 * post + 4 * (t - 1);
}
constexpr uint32_t status_bit(unsigned post) { return 1u << (30 + post); }

} // namespace

TEST_CASE("pwm_bk7231n has three groups in one range; time registers read back") {
    Fixture f;
    CHECK(f.pwm.name() == "pwm");
    CHECK(f.pwm.ranges().at(0).name == "regs");
    CHECK(f.pwm.ranges().at(0).size == 0xC0);
    f.w(time_reg(2, 1, 4), 0xA944);
    f.w(time_reg(0, 0, 1), 0x1234);
    CHECK(f.r(time_reg(2, 1, 4)) == 0xA944);
    CHECK(f.r(time_reg(0, 0, 1)) == 0x1234);
    CHECK(f.r(time_reg(1, 0, 1)) == 0);
}

TEST_CASE("pwm_bk7231n control reads back without the undefined bits") {
    Fixture f;
    f.w(ctrl(1), 0x3C000000 | 0x03123456);
    CHECK(f.r(ctrl(1)) == 0x03123456);
    CHECK(f.r(ctrl(0)) == 0);
}

TEST_CASE("pwm_bk7231n a read request completes at once and reads 0") {
    Fixture f;
    f.w(ctrl(0) + 0x24, 3);
    CHECK(f.r(ctrl(0) + 0x24) == 0);
    CHECK(f.r(ctrl(0) + 0x28) == 0);
    CHECK(f.r(ctrl(0) + 0x2C) == 0);
}

TEST_CASE("pwm_bk7231n a channel ends a period every T4 clocks") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(ctrl(0), on(0));
    REQUIRE(f.pwm.next_event_ns());
    CHECK(*f.pwm.next_event_ns() == 100000);
    f.advance(99999);
    CHECK((f.r(ctrl(0)) & status_bit(0)) == 0);
    f.advance(1);
    CHECK((f.r(ctrl(0)) & status_bit(0)) != 0);
    CHECK(f.calls.back());
    CHECK(*f.pwm.next_event_ns() == 200000);
}

TEST_CASE("pwm_bk7231n timer mode runs, idle and capture modes do not") {
    for (uint32_t mode : {2u, 0u, 3u, 4u, 5u, 6u}) {
        Fixture f;
        f.w(time_reg(0, 0, 4), 100);
        f.w(ctrl(0), on(0, mode));
        CHECK(f.pwm.next_event_ns().has_value() == (mode == 2));
    }
}

TEST_CASE("pwm_bk7231n needs the enable bit and a period") {
    Fixture f;
    f.w(ctrl(0), on(0));
    CHECK_FALSE(f.pwm.next_event_ns());
    f.w(time_reg(0, 0, 4), 100);
    CHECK(f.pwm.next_event_ns());
    f.w(ctrl(0), 1 | 1u << 4);
    CHECK_FALSE(f.pwm.next_event_ns());
}

TEST_CASE("pwm_bk7231n without its interrupt enabled a channel sets no status") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(ctrl(0), on(0, 1, false));
    f.advance(100000);
    CHECK((f.r(ctrl(0)) & status_bit(0)) == 0);
    CHECK_FALSE(f.calls.back());
    CHECK(*f.pwm.next_event_ns() == 200000);
}

TEST_CASE("pwm_bk7231n status is write 1 to clear and lowers the line when none is left") {
    Fixture f(1000000);
    f.w(time_reg(1, 0, 4), 100);
    f.w(time_reg(1, 1, 4), 100);
    f.w(ctrl(1), on(0) | on(1));
    f.advance(100000);
    CHECK((f.r(ctrl(1)) & (status_bit(0) | status_bit(1))) == (status_bit(0) | status_bit(1)));
    // The way the vendor driver clears one: the control value with one bit set.
    f.w(ctrl(1), on(0) | on(1) | status_bit(0));
    CHECK((f.r(ctrl(1)) & (status_bit(0) | status_bit(1))) == status_bit(1));
    CHECK(f.calls.back());
    f.w(ctrl(1), on(0) | on(1) | status_bit(1));
    CHECK((f.r(ctrl(1)) & (status_bit(0) | status_bit(1))) == 0);
    CHECK_FALSE(f.calls.back());
}

TEST_CASE("pwm_bk7231n a group clears only its own status") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(time_reg(1, 0, 4), 100);
    f.w(ctrl(0), on(0));
    f.w(ctrl(1), on(0));
    f.advance(100000);
    f.w(ctrl(0), on(0) | status_bit(0));
    CHECK((f.r(ctrl(0)) & status_bit(0)) == 0);
    CHECK((f.r(ctrl(1)) & status_bit(0)) != 0);
    CHECK(f.calls.back());
}

TEST_CASE("pwm_bk7231n turning the interrupt off drops the channel's status") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(ctrl(0), on(0));
    f.advance(100000);
    CHECK(f.calls.back());
    f.w(ctrl(0), on(0, 1, false));
    CHECK((f.r(ctrl(0)) & status_bit(0)) == 0);
    CHECK_FALSE(f.calls.back());
}

TEST_CASE("pwm_bk7231n a running channel keeps its schedule across a control write") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(ctrl(0), on(0));
    f.clock.now = 40000;
    f.w(ctrl(0), on(0) | 1u << 7);
    CHECK(*f.pwm.next_event_ns() == 100000);
}

TEST_CASE("pwm_bk7231n a late tick skips the missed periods") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(ctrl(0), on(0));
    f.advance(350000);
    CHECK(*f.pwm.next_event_ns() == 400000);
}

TEST_CASE("pwm_bk7231n reset stops the channels and lowers the line") {
    Fixture f(1000000);
    f.w(time_reg(0, 0, 4), 100);
    f.w(ctrl(0), on(0));
    f.advance(100000);
    f.pwm.reset();
    CHECK_FALSE(f.calls.back());
    CHECK_FALSE(f.pwm.next_event_ns());
    CHECK(f.r(ctrl(0)) == 0);
    CHECK(f.r(time_reg(0, 0, 4)) == 0);
}
