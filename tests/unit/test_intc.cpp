#include <doctest/doctest.h>

#include <map>
#include <stdexcept>
#include <string>

#include "periph/intc.h"

using namespace bk;

namespace {

struct Fixture {
    bool irq = false, fiq = false;
    int  irq_calls = 0, fiq_calls = 0;
    Intc intc;

    explicit Fixture(std::map<std::string, SignalSpec> signals =
                         {
    })
        : intc(
              "intc",
              [this](bool v) {
                  irq = v;
                  ++irq_calls;
              },
              [this](bool v) {
                  fiq = v;
                  ++fiq_calls;
              },
              {{0x08, 0x00ffffff}},
              std::move(signals)) {}

    void     w(uint32_t off, uint32_t v) { intc.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return intc.read("regs", off, 4); }
};

} // namespace

TEST_CASE("irq asserted when enabled and global bit0") {
    Fixture f;
    Line    l = f.intc.line(3);
    l(true);
    CHECK_FALSE(f.irq);
    f.w(0x40, 8);
    CHECK_FALSE(f.irq);
    f.w(0x44, 1);
    CHECK(f.irq);
    f.w(0x44, 0);
    CHECK_FALSE(f.irq);
    CHECK(f.intc.ranges().at(0).name == "regs");
    CHECK(f.intc.ranges().at(0).size == 0x54);
}

TEST_CASE("fiq uses bits 16-31 and global bit1") {
    Fixture f;
    Line    l = f.intc.line(17);
    l(true);
    f.w(0x40, 1u << 17);
    f.w(0x44, 1);
    CHECK_FALSE(f.fiq);
    CHECK_FALSE(f.irq);
    f.w(0x44, 2);
    CHECK(f.fiq);
    CHECK_FALSE(f.irq);
    CHECK(f.r(0x44) == 2);
    f.w(0x44, 0xffffffff);
    CHECK(f.r(0x44) == 3);
}

TEST_CASE("status 0x4C is raw & enable, write 1 clears latched") {
    Fixture f;
    Line    l = f.intc.line(5);
    f.w(0x40, 1u << 5);
    l(true);
    l(false);
    CHECK(f.r(0x48) == (1u << 5));
    CHECK(f.r(0x4C) == (1u << 5));
    f.w(0x40, 0);
    CHECK(f.r(0x48) == (1u << 5));
    CHECK(f.r(0x4C) == 0);
    f.w(0x4C, 1u << 5);
    CHECK(f.r(0x48) == 0);
    l(true);
    l(false);
    f.w(0x48, 1u << 5);
    CHECK(f.r(0x48) == 0);
}

TEST_CASE("latched bit returns while level high") {
    Fixture f;
    Line    l = f.intc.line(2);
    l(true);
    f.w(0x4C, 4);
    CHECK(f.r(0x48) == 4);
    l(false);
    CHECK(f.r(0x48) == 0);
}

TEST_CASE("two lines share one bit") {
    Fixture f;
    Line    a = f.intc.line(4);
    Line    b = f.intc.line(4);
    f.w(0x40, 0x10);
    f.w(0x44, 1);
    a(true);
    b(true);
    CHECK(f.irq);
    a(false);
    CHECK(f.irq);
    f.w(0x48, 0x10); // latched cleared, b still high
    CHECK(f.r(0x48) == 0x10);
    b(false);
    CHECK(f.r(0x48) == 0);
    CHECK_FALSE(f.irq);
}

TEST_CASE("cpu lines recomputed on every update") {
    Fixture f;
    Line    l      = f.intc.line(0);
    int     before = f.irq_calls;
    l(false);
    CHECK(f.irq_calls == before + 1);
    CHECK(f.fiq_calls > 0);
}

TEST_CASE("reset words and reset") {
    Fixture f;
    CHECK(f.r(0x08) == 0x00ffffff);
    f.w(0x08, 5);
    f.w(0x40, 0xff);
    f.w(0x44, 3);
    Line l = f.intc.line(1);
    l(true);
    l(false);
    CHECK(f.r(0x08) == 5);
    f.intc.reset();
    CHECK(f.r(0x08) == 0x00ffffff);
    CHECK(f.r(0x40) == 0);
    CHECK(f.r(0x44) == 0);
    CHECK(f.r(0x48) == 0);
    CHECK_FALSE(f.irq);
}

TEST_CASE("signal arm_wdt_pwd") {
    Fixture f({
        {"arm_wdt_pwd", SignalSpec{0x08, 8}}
    });
    Signal  s = f.intc.signal("arm_wdt_pwd");
    CHECK(s()); // 0x00ffffff has bit 8
    f.w(0x08, 0);
    CHECK_FALSE(s());
    f.w(0x08, 0x100);
    CHECK(s());
    CHECK_THROWS_AS(f.intc.signal("nope"), std::invalid_argument);
}

TEST_CASE("signal width 32 and equals") {
    Fixture f({
        {"full",  SignalSpec{0x20, 0, 32, 0xdeadbeef}},
        {"field", SignalSpec{0x24, 4, 3, 5}          }
    });
    Signal  full  = f.intc.signal("full");
    Signal  field = f.intc.signal("field");
    CHECK_FALSE(full());
    f.w(0x20, 0xdeadbeef);
    CHECK(full());
    f.w(0x20, 0xdeadbeee);
    CHECK_FALSE(full());
    f.w(0x24, 5u << 4);
    CHECK(field());
    f.w(0x24, (5u << 4) | 0xf0000000u | 0xf);
    CHECK(field());
    f.w(0x24, 7u << 4);
    CHECK_FALSE(field());
}

TEST_CASE("unknown offset stores plain word") {
    Fixture f;
    CHECK(f.r(0x10) == 0);
    f.w(0x10, 0x12345678);
    CHECK(f.r(0x10) == 0x12345678);
    CHECK(f.r(0x14) == 0);
}
