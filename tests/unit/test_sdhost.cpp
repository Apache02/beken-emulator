#include <doctest/doctest.h>

#include <sstream>

#include "core/log.h"
#include "periph/sdhost.h"

using namespace bk;

namespace {

struct Fixture {
    std::ostringstream out;
    SdHost             host;

    Fixture()
        : host("SDCARD",
               0x100,
               {
                   {0x08, 0xffffffff}
    },
               nullptr) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    uint32_t r(uint32_t off) { return host.read("regs", off, 4); }
    void     w(uint32_t off, uint32_t v) { host.write("regs", off, 4, v); }
};

} // namespace

TEST_CASE("sdhost: a command with START times out, as with no card") {
    Fixture f;
    CHECK(f.r(0x24) == 0);
    f.w(0x00, 0x00000001);
    CHECK(f.r(0x24) == 0x4);
    // The control word is kept as written.
    CHECK(f.r(0x00) == 0x00000001);
}

TEST_CASE("sdhost: a write without START raises nothing") {
    Fixture f;
    f.w(0x00, 0x00000002);
    f.w(0x00, 0xFFFFFFFE);
    CHECK(f.r(0x24) == 0);
}

TEST_CASE("sdhost: a write to the status register clears the bits written") {
    Fixture f;
    f.w(0x00, 1);
    REQUIRE(f.r(0x24) == 0x4);
    // Bits not set in the value stay; writing zero clears nothing.
    f.w(0x24, 0x00000001);
    CHECK(f.r(0x24) == 0x4);
    f.w(0x24, 0);
    CHECK(f.r(0x24) == 0x4);
    f.w(0x24, 0x4);
    CHECK(f.r(0x24) == 0);
    // Each START raises it again.
    f.w(0x00, 1);
    CHECK(f.r(0x24) == 0x4);
    f.w(0x24, 0xFFFFFFFF);
    CHECK(f.r(0x24) == 0);
}

TEST_CASE("sdhost: other registers keep their values and start from the reset words") {
    Fixture f;
    CHECK(f.r(0x08) == 0xffffffff);
    CHECK(f.r(0x10) == 0);
    f.w(0x10, 0x1234);
    f.w(0x30, 0xabcd);
    CHECK(f.r(0x10) == 0x1234);
    CHECK(f.r(0x30) == 0xabcd);
    f.host.reset();
    CHECK(f.r(0x10) == 0);
    CHECK(f.r(0x08) == 0xffffffff);
    CHECK(f.r(0x24) == 0);
}
