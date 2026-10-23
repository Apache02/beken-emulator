#include <doctest/doctest.h>

#include <optional>
#include <sstream>
#include <string>

#include "core/log.h"
#include "periph/wifi_mac.h"

using namespace bk;

namespace {

constexpr uint32_t kCounter1 = 0x11C, kLo = 0x120, kHi = 0x124, kSwUpdate = 1u << 31;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

struct Fixture {
    std::ostringstream out;
    FakeClock          clock;
    bool               power = true;
    WifiMac            mac;

    explicit Fixture(bool with_signal = true)
        : mac("MAC", clock, with_signal ? Signal([this] { return power; }) : Signal(), nullptr) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    uint32_t core(uint32_t off, uint32_t size = 4) { return mac.read("core", off, size); }
    void     wcore(uint32_t off, uint32_t v, uint32_t size = 4) { mac.write("core", off, size, v); }
};

} // namespace

TEST_CASE("mac has the core and platform ranges") {
    Fixture f;
    REQUIRE(f.mac.ranges().size() == 2);
    CHECK(f.mac.ranges()[0].name == "core");
    CHECK(f.mac.ranges()[0].size == 0x1580);
    CHECK(f.mac.ranges()[1].name == "pl");
    CHECK(f.mac.ranges()[1].size == 0x57C);
}

TEST_CASE("mac counters read 0 while powered down") {
    Fixture f;
    f.power     = false;
    f.clock.now = 5'000'000;
    CHECK(f.core(kCounter1) == 0);
    CHECK(f.core(kLo) == 0);
    CHECK(f.core(kHi) == 0);
    CHECK_FALSE(f.mac.microseconds().has_value());
}

TEST_CASE("mac counters count from the moment the MAC powers up") {
    Fixture f;
    f.power     = false;
    f.clock.now = 1'000'000;
    f.mac.tick(f.clock.now);
    f.power     = true;
    f.clock.now = 2'000'000;
    f.mac.tick(f.clock.now); // power-up noticed here: elapsed restarts at 0
    CHECK(f.core(kCounter1) == 0);
    f.clock.now = 2'000'000 + 10'000; // 10 us
    CHECK(f.core(kCounter1) == 400);  // 40 MHz
    CHECK(f.core(kLo) == 10);
    CHECK(f.core(kHi) == 0);
    CHECK(f.mac.microseconds() == std::optional<uint64_t>(10));
}

TEST_CASE("mac counter 1 wraps at 32 bits") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 107'374'183'000ULL; // 40 * 107374183 us = 4294967320 -> 24 after the wrap
    CHECK(f.core(kCounter1) == static_cast<uint32_t>(107'374'183ULL * 40));
}

TEST_CASE("mac counter 2 splits the microsecond count into lo and hi") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = (uint64_t{0x1234} << 32 | 0x89ABCDEFu) * 1000;
    CHECK(f.core(kLo) == 0x89ABCDEF);
    CHECK(f.core(kHi) == 0x1234);
}

TEST_CASE("mac counters ignore writes except the software update") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 50'000;
    f.wcore(kCounter1, 0);
    f.wcore(kLo, 1000);
    f.wcore(kHi, 0x5); // no update flag: nothing happens
    CHECK(f.core(kLo) == 50);
    CHECK(f.core(kHi) == 0);
    CHECK(f.core(kCounter1) == 2000);
}

TEST_CASE("mac software update loads counter 2 from the last lo write and hi") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 7'000; // 7 us elapsed
    f.wcore(kLo, 0x00001000);
    f.wcore(kHi, kSwUpdate | 0x0002);
    CHECK(f.core(kLo) == 0x1000);
    CHECK(f.core(kHi) == 0x0002); // bit 31 reads 0
    f.clock.now = 17'000;         // 10 us later
    CHECK(f.core(kLo) == 0x100A);
    // The target may be below the elapsed time: modular arithmetic.
    f.wcore(kLo, 3);
    f.wcore(kHi, kSwUpdate);
    CHECK(f.core(kLo) == 3);
    f.clock.now = 27'000;
    CHECK(f.core(kLo) == 13);
    // Counter 1 is not affected by the update.
    CHECK(f.core(kCounter1) == 1080);
}

TEST_CASE("mac software update below elapsed time wraps modulo 2^48") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 100'000; // 100 us
    f.wcore(kLo, 0);
    f.wcore(kHi, kSwUpdate); // target 0
    CHECK(f.core(kLo) == 0);
    f.clock.now = 150'000;
    CHECK(f.core(kLo) == 50);
}

TEST_CASE("mac software update while powered down does nothing; power-down drops the offset") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 1'000;
    f.wcore(kLo, 500);
    f.wcore(kHi, kSwUpdate);
    CHECK(f.core(kLo) == 500);
    f.power = false;
    f.mac.tick(2'000);
    f.wcore(kLo, 900);
    f.wcore(kHi, kSwUpdate); // powered down: ignored
    f.power     = true;
    f.clock.now = 10'000;
    f.mac.tick(10'000); // powered up again at 10 us
    f.clock.now = 14'000;
    CHECK(f.core(kLo) == 4);
}

TEST_CASE("mac without a power signal is always powered and counts from 0") {
    Fixture f(false);
    f.clock.now = 25'000;
    CHECK(f.core(kLo) == 25);
    CHECK(f.core(kCounter1) == 1000);
}

TEST_CASE("mac the firmware's counter-running probe sees the counter move") {
    Fixture f;
    f.mac.tick(0);
    // `time_counter_is_running`: read counter 1, read it 16 more times, expect a change. Every read
    // here costs a little device time.
    const uint32_t first = f.core(kCounter1);
    bool           moved = false;
    for (int i = 0; i < 16; ++i) {
        f.clock.now += 100; // 100 ns = 4 ticks at 40 MHz
        if (f.core(kCounter1) != first) moved = true;
    }
    CHECK(moved);
    // Powered down: it never moves.
    f.power = false;
    f.mac.tick(f.clock.now);
    CHECK(f.core(kCounter1) == 0);
    f.clock.now += 1000;
    CHECK(f.core(kCounter1) == 0);
}

TEST_CASE("mac other registers are logged stand-ins that read back what was written") {
    Fixture f;
    f.wcore(0x04, 0xDEADBEEF);
    CHECK(f.core(0x04) == 0xDEADBEEF);
    f.mac.write("pl", 0x6C, 4, 0x1234);
    CHECK(f.mac.read("pl", 0x6C, 4) == 0x1234);
    CHECK(f.out.str().find("MAC_CORE write +0x0004 (4 bytes) = 0xDEADBEEF") != std::string::npos);
    CHECK(f.out.str().find("MAC_PL read +0x006C (4 bytes) = 0x00001234") != std::string::npos);
    // The counters are not logged.
    f.out.str("");
    f.core(kCounter1);
    f.core(kLo);
    CHECK(f.out.str().empty());
}

TEST_CASE("mac reset zeroes the stand-ins and restarts the counters") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 9'000;
    f.wcore(kLo, 100);
    f.wcore(kHi, kSwUpdate);
    f.wcore(0x04, 7);
    f.mac.reset();
    CHECK(f.core(0x04) == 0);
    // Not yet noticed as powered: the first access starts the count.
    CHECK(f.core(kLo) == 0);
    f.clock.now = 12'000;
    CHECK(f.core(kLo) == 3);
}
