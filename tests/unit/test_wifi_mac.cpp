#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <span>
#include <sstream>
#include <string>

#include "core/log.h"
#include "periph/wifi/mac.h"

using namespace bk;

namespace {

constexpr uint32_t kCounter1 = 0x11C, kLo = 0x120, kHi = 0x124, kSwUpdate = 1u << 31;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

// The frame engine is covered in test_wifi_frame_engine.cpp: here nothing
// is reachable by the bus master.
struct NoMemory : BusMemory {
    std::vector<uint8_t> read(uint32_t addr, uint32_t) override { throw BusError(addr, "no memory"); }
    void                 write(uint32_t addr, std::span<const uint8_t>) override { throw BusError(addr, "no memory"); }
};

struct Fixture {
    // Line indexes in `level`/`writes`.
    enum { Timer, Misc, Rx, Tx, Prot, General };

    std::ostringstream out;
    FakeClock          clock;
    NoMemory           memory;
    bool               power = true;
    bool               level[6]{};
    int                writes[6]{};
    WifiMac            mac;

    explicit Fixture(bool with_signal = true)
        : mac("MAC",
              clock,
              with_signal ? Signal([this] { return power; }) : Signal(),
              WifiMac::Lines{line(Timer), line(Misc), line(Rx), line(Tx), line(Prot), line(General)},
              nullptr,
              memory) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
    }
    ~Fixture() { Logger::reset_for_tests(); }

    Line line(int i) {
        return [this, i](bool v) {
            level[i] = v;
            ++writes[i];
        };
    }
    uint32_t core(uint32_t off, uint32_t size = 4) { return mac.read("core", off, size); }
    void     wcore(uint32_t off, uint32_t v, uint32_t size = 4) { mac.write("core", off, size, v); }
    uint32_t pl(uint32_t off) { return mac.read("pl", off, 4); }
    void     wpl(uint32_t off, uint32_t v) { mac.write("pl", off, 4, v); }
    // Advances the clock to `us` microseconds and ticks.
    void at_us(uint64_t us) {
        clock.now = us * 1000;
        mac.tick(clock.now);
    }
};

} // namespace

TEST_CASE("mac has the core, platform and PTA ranges") {
    Fixture f;
    REQUIRE(f.mac.ranges().size() == 3);
    CHECK(f.mac.ranges()[0].name == "core");
    CHECK(f.mac.ranges()[0].size == 0x1580);
    CHECK(f.mac.ranges()[1].name == "pl");
    CHECK(f.mac.ranges()[1].size == 0x57C);
    CHECK(f.mac.ranges()[2].name == "pta");
    CHECK(f.mac.ranges()[2].size == 40);
}

TEST_CASE("mac PTA bank holds what is written and is cleared by a reset") {
    Fixture f;
    f.mac.write("pta", 0x10, 4, 0xCAFE);
    CHECK(f.mac.read("pta", 0x10, 4) == 0xCAFE);
    // The other banks are not touched.
    CHECK(f.pl(0x10) == 0);
    f.mac.reset();
    CHECK(f.mac.read("pta", 0x10, 4) == 0);
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

TEST_CASE("mac other registers are stand-ins that read back what was written") {
    Fixture f;
    f.wcore(0x100, 0xDEADBEEF);
    CHECK(f.core(0x100) == 0xDEADBEEF);
    f.mac.write("pl", 0x100, 4, 0x1234);
    CHECK(f.mac.read("pl", 0x100, 4) == 0x1234);
}

TEST_CASE("mac reset zeroes the stand-ins and restarts the counters") {
    Fixture f;
    f.mac.tick(0);
    f.clock.now = 9'000;
    f.wcore(kLo, 100);
    f.wcore(kHi, kSwUpdate);
    f.wcore(0x100, 7);
    f.mac.reset();
    CHECK(f.core(0x100) == 0);
    // Not yet noticed as powered: the first access starts the count.
    CHECK(f.core(kLo) == 0);
    f.clock.now = 12'000;
    CHECK(f.core(kLo) == 3);
}

namespace {

constexpr uint32_t kState = 0x038, kErrRec = 0x054, kBeacon1 = 0x064, kKey0 = 0x0AC, kAddrLo = 0x0BC, kAddrHi = 0x0C0,
                   kEncr = 0x0C4, kKeyRange = 0x0D8, kAbs0 = 0x128;
constexpr uint32_t kDoze2 = 0x048, kMacCntrl2 = 0x050, kGenStatus = 0x06C, kGenAck = 0x070, kGenEnable = 0x074,
                   kTxStatus = 0x078, kTxAck = 0x07C, kTxEnable = 0x080, kEventSet = 0x084, kEventClear = 0x088,
                   kUnmask = 0x08C, kTsfLo = 0x0A4, kTsfHi = 0x0A8, kAir1 = 0x160, kAir2 = 0x164, kAirValue = 0x168;
constexpr uint32_t kMaster = 1u << 31, kTbtt = 1, kIdleBit = 4, kTimersBit = 8;

} // namespace

TEST_CASE("mac version reads 0x1C03 and ignores writes") {
    Fixture f;
    CHECK(f.core(0x004) == 0x00001C03);
    f.wcore(0x004, 0);
    CHECK(f.core(0x004) == 0x00001C03);
}

TEST_CASE("mac state control stores next state and state, IDLE sets the general interrupt") {
    Fixture f;
    f.wpl(kGenEnable, 0xFFFFFFFF);
    CHECK(f.core(kState) == 0);
    f.wcore(kState, 0x30);
    CHECK(f.core(kState) == 0x33);
    CHECK(f.pl(kGenStatus) == 0);
    f.wcore(kState, 0x00); // ACTIVE -> IDLE
    CHECK(f.core(kState) == 0);
    CHECK(f.pl(kGenStatus) == kIdleBit);
    f.wpl(kGenAck, kIdleBit);
    f.wcore(kState, 0x00); // IDLE -> IDLE
    CHECK(f.pl(kGenStatus) == 0);
    f.wcore(kState, 0x20);
    CHECK(f.core(kState) == 0x22);
}

TEST_CASE("mac error recovery control drops the reset bits") {
    Fixture f;
    f.wcore(kErrRec, 0xFFFFFFFF);
    CHECK(f.core(kErrRec) == 0xFFFFFF03);
}

TEST_CASE("mac general interrupts: status, acknowledge, enable and the line") {
    Fixture f;
    f.at_us(0);
    f.wcore(kState, 0x30);
    f.wcore(kState, 0x00);        // sets IDLE
    CHECK(f.pl(kGenStatus) == 0); // not enabled
    CHECK_FALSE(f.level[Fixture::General]);
    f.wpl(kGenEnable, kIdleBit); // no master enable yet
    CHECK(f.pl(kGenStatus) == kIdleBit);
    CHECK_FALSE(f.level[Fixture::General]);
    f.wpl(kGenEnable, kMaster | kIdleBit);
    CHECK(f.level[Fixture::General]);
    CHECK(f.pl(kGenStatus) == kIdleBit); // bit 31 never shows in the status
    f.wpl(kGenAck, kIdleBit);
    CHECK(f.pl(kGenStatus) == 0);
    CHECK_FALSE(f.level[Fixture::General]);
}

TEST_CASE("mac tx/rx interrupts route status bits to the five lines") {
    Fixture f;
    f.wpl(kTxEnable, 0xFFFFFFFF);
    struct Case {
        uint32_t bits;
        int      line; // -1: none
    };
    const Case cases[] = {
        {1u << 0,  Fixture::Prot },
        {1u << 5,  Fixture::Prot },
        {1u << 6,  Fixture::Tx   },
        {1u << 12, Fixture::Tx   },
        {1u << 15, Fixture::Tx   },
        {1u << 13, Fixture::Misc },
        {1u << 14, Fixture::Timer},
        {1u << 16, Fixture::Rx   },
        {1u << 17, Fixture::Timer},
        {1u << 18, Fixture::Misc },
        {1u << 28, Fixture::Misc },
        {1u << 29, -1            },
    };
    for (const Case &c : cases) {
        INFO("bits 0x" << std::hex << c.bits);
        f.mac.raise_txrx(c.bits);
        for (int i = 0; i < 6; ++i)
            if (i != Fixture::General) CHECK(f.level[i] == (i == c.line));
        f.wpl(kTxAck, c.bits);
        for (int i = 0; i < 6; ++i) CHECK_FALSE(f.level[i]);
    }
}

TEST_CASE("mac tx/rx status honours the enable and the master bit") {
    Fixture f;
    f.mac.raise_txrx(0x80000041);
    f.wpl(kTxEnable, 0x7FFFFFFF); // no master
    CHECK(f.pl(kTxStatus) == 0x41);
    CHECK_FALSE(f.level[Fixture::Prot]);
    CHECK_FALSE(f.level[Fixture::Tx]);
    f.wpl(kTxEnable, kMaster | 0x40);
    CHECK(f.pl(kTxStatus) == 0x40);
    CHECK(f.level[Fixture::Tx]);
    CHECK_FALSE(f.level[Fixture::Prot]);
}

TEST_CASE("mac writes an ICU line only when its level changed") {
    Fixture f;
    f.wpl(kTxEnable, 0xFFFFFFFF);
    const int before = f.writes[Fixture::Prot];
    f.mac.raise_txrx(1);
    f.mac.raise_txrx(2);
    f.wpl(kTxEnable, 0xFFFFFFFF);
    CHECK(f.writes[Fixture::Prot] == before + 1);
    f.wpl(kTxAck, 3);
    CHECK(f.writes[Fixture::Prot] == before + 2);
}

TEST_CASE("mac ABS timers fire in tick, honour the unmask register and set TIMERS") {
    Fixture f;
    f.at_us(0);
    f.wpl(kGenEnable, kMaster | kTimersBit);
    f.wpl(kUnmask, 1);
    f.wcore(kAbs0, 100);
    f.wcore(kAbs0 + 4, 200); // timer 1 is masked
    f.at_us(99);
    CHECK(f.pl(kEventSet) == 0);
    CHECK_FALSE(f.level[Fixture::General]);
    f.at_us(100);
    CHECK(f.pl(kEventSet) == 1);
    CHECK(f.pl(kEventClear) == 1);
    CHECK(f.pl(kGenStatus) == kTimersBit);
    CHECK(f.level[Fixture::General]);
    f.wpl(kEventClear, 1);
    CHECK(f.pl(kEventSet) == 0);
    CHECK(f.pl(kEventClear) == 0);
    f.at_us(200); // masked: expires silently
    CHECK(f.pl(kEventSet) == 0);
    f.at_us(300);
    CHECK(f.pl(kEventSet) == 0);
    CHECK_FALSE(f.mac.next_event_ns().has_value());
}

TEST_CASE("mac timer events, doze control and TSF are platform bank registers") {
    // The core bank words at the same offsets are reserved: plain stand-ins.
    Fixture f;
    f.at_us(0);
    f.wpl(kGenEnable, kMaster | kTimersBit);
    f.wcore(kUnmask, 1);
    f.wcore(kAbs0, 100);
    f.at_us(100);
    CHECK(f.pl(kEventSet) == 0); // fired, but masked in the platform bank
    CHECK(f.core(kUnmask) == 1);
    f.wcore(kEventSet, 1);
    CHECK(f.pl(kEventSet) == 0);
    CHECK(f.core(kEventSet) == 1);
    CHECK_FALSE(f.level[Fixture::General]);
    f.at_us(200);
    CHECK(f.pl(kTsfLo) == 200);
    CHECK(f.core(kTsfLo) == 0);
    f.wcore(kTsfLo, 7);
    CHECK(f.pl(kTsfLo) == 200);
    f.wcore(kState, 0x20);
    f.wcore(kDoze2, 1);
    CHECK(f.core(kState) == 0x22);
}

TEST_CASE("mac ABS timer targets at or behind the counter never match") {
    Fixture f;
    f.at_us(0);
    f.wpl(kUnmask, 0x3FF);
    f.at_us(500);
    f.wcore(kAbs0, 500);                   // equal
    f.wcore(kAbs0 + 4, 400);               // behind
    f.wcore(kAbs0 + 8, 500 + 0x80000000u); // half the range ahead is not "ahead"
    f.at_us(600);
    CHECK(f.pl(kEventSet) == 0);
    CHECK_FALSE(f.mac.next_event_ns().has_value());
    f.wcore(kAbs0 + 36, 700); // the last timer
    f.at_us(700);
    CHECK(f.pl(kEventSet) == 0x200);
}

TEST_CASE("mac event_set forces events and raises TIMERS only for unmasked ones") {
    Fixture f;
    f.wpl(kGenEnable, kMaster | kTimersBit);
    f.wpl(kUnmask, 1);
    f.wpl(kEventSet, 0xFFFFFC02); // only bits 0..9 count; timer 1 is masked
    CHECK(f.pl(kEventSet) == 0x2);
    CHECK(f.pl(kEventClear) == 0x2);
    CHECK(f.pl(kGenStatus) == 0);
    f.wpl(kEventSet, 0x401);
    CHECK(f.pl(kEventSet) == 0x3);
    CHECK(f.pl(kGenStatus) == kTimersBit);
}

TEST_CASE("mac next event is the earliest armed ABS timer") {
    Fixture f;
    f.at_us(0);
    CHECK_FALSE(f.mac.next_event_ns().has_value());
    f.wcore(kAbs0 + 8, 300);
    f.wcore(kAbs0, 100);
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(100'000));
    f.at_us(40);
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(100'000));
    f.power = false;
    CHECK_FALSE(f.mac.next_event_ns().has_value());
}

TEST_CASE("mac TSF follows the counter, writes move one half at a time") {
    Fixture f;
    f.at_us(0);
    f.at_us(10);
    CHECK(f.pl(kTsfLo) == 10);
    CHECK(f.pl(kTsfHi) == 0);
    f.wpl(kTsfLo, 0x1000);
    CHECK(f.pl(kTsfLo) == 0x1000);
    f.at_us(20);
    CHECK(f.pl(kTsfLo) == 0x100A);
    f.wpl(kTsfHi, 5);
    CHECK(f.pl(kTsfLo) == 0x100A);
    CHECK(f.pl(kTsfHi) == 5);
    CHECK(f.core(kLo) == 20); // the counter itself is not touched
    f.wpl(kTsfLo, 0xFFFFFFF0);
    f.at_us(40); // 20 us later: the 64-bit sum carries into the high half
    CHECK(f.pl(kTsfLo) == 4);
    CHECK(f.pl(kTsfHi) == 6);
    f.power = false;
    CHECK(f.pl(kTsfLo) == 0);
    CHECK(f.pl(kTsfHi) == 0);
}

TEST_CASE("mac TBTT sets the general interrupt once per beacon period") {
    Fixture f;
    f.at_us(0);
    f.wpl(kGenEnable, kMaster | kTbtt);
    f.wcore(kBeacon1, 0x10001); // only the low 16 bits count: 1 * 1024 us
    f.at_us(500);
    f.wcore(kState, 0x30);
    f.at_us(600); // the first tick only remembers the index
    CHECK(f.pl(kGenStatus) == 0);
    f.at_us(1023);
    CHECK(f.pl(kGenStatus) == 0);
    CHECK(f.mac.next_event_ns() == std::optional<uint64_t>(1'024'000));
    f.at_us(1024);
    CHECK(f.pl(kGenStatus) == kTbtt);
    CHECK(f.level[Fixture::General]);
    f.wpl(kGenAck, kTbtt);
    f.at_us(1500);
    CHECK(f.pl(kGenStatus) == 0);
    f.at_us(2048);
    CHECK(f.pl(kGenStatus) == kTbtt);
}

TEST_CASE("mac TBTT does nothing in IDLE or without a period") {
    Fixture f;
    f.at_us(0);
    f.wpl(kGenEnable, kMaster | kTbtt);
    f.wcore(kBeacon1, 1);
    f.at_us(0);
    f.at_us(5000);
    CHECK(f.pl(kGenStatus) == 0); // IDLE
    CHECK_FALSE(f.mac.next_event_ns().has_value());
    f.wcore(kState, 0x30);
    f.wcore(kBeacon1, 0);
    f.at_us(6000);
    f.at_us(9000);
    CHECK(f.pl(kGenStatus) == 0);
    CHECK_FALSE(f.mac.next_event_ns().has_value());
}

TEST_CASE("mac TBTT wakes DOZE; entering ACTIVE from DOZE keeps the beacon index, from elsewhere forgets it") {
    Fixture f;
    f.at_us(0);
    f.wpl(kGenEnable, kMaster | kTbtt);
    f.wcore(kBeacon1, 1);
    f.wcore(kState, 0x20); // DOZE
    f.at_us(100);          // remembers index 0
    f.at_us(1100);         // index 1
    CHECK(f.pl(kGenStatus) == kTbtt);
    CHECK(f.core(kState) == 0x33); // woken
    f.wpl(kGenAck, kTbtt);
    // ACTIVE -> ACTIVE forgets the index: the next tick only remembers (index 1 again, at 1200)
    f.wcore(kState, 0x30);
    f.at_us(1200);
    f.at_us(2000);
    CHECK(f.pl(kGenStatus) == 0);
    f.at_us(2100);
    CHECK(f.pl(kGenStatus) == kTbtt);
    f.wpl(kGenAck, kTbtt);
    // DOZE -> ACTIVE keeps it
    f.wcore(kState, 0x20);
    f.wcore(kState, 0x30);
    f.at_us(3100);
    CHECK(f.pl(kGenStatus) == kTbtt);
}

TEST_CASE("mac doze control bit 0 wakes a dozing MAC") {
    Fixture f;
    f.wcore(kState, 0x20);
    f.wpl(kDoze2, 0);
    CHECK(f.core(kState) == 0x22);
    f.wpl(kDoze2, 1);
    CHECK(f.core(kState) == 0x33);
    f.wcore(kState, 0x00);
    f.wpl(kDoze2, 1); // only DOZE is affected
    CHECK(f.core(kState) == 0);
    CHECK(f.pl(kDoze2) == 1);
}

TEST_CASE("mac key storage: write, read back, search in insertion order") {
    Fixture f;
    auto    set_key = [&](uint32_t lo, uint32_t hi, uint32_t w0) {
        f.wcore(kKey0, w0);
        f.wcore(kKey0 + 4, w0 + 1);
        f.wcore(kKey0 + 8, w0 + 2);
        f.wcore(kKey0 + 12, w0 + 3);
        f.wcore(kAddrLo, lo);
        f.wcore(kAddrHi, hi);
    };
    set_key(0x44332211, 0x6655, 1);
    f.wcore(kEncr, 1u << 30 | 5u << 16 | 0x123);
    CHECK(f.core(kEncr) == (5u << 16 | 0x123)); // the command bit reads back 0
    set_key(0x44332211, 0x6655, 100);
    f.wcore(kEncr, 1u << 30 | 3u << 16 | 0x7FF | 0x800); // attributes keep 11 bits
    // Read entry 5: registers and attributes come back.
    set_key(0, 0, 0);
    f.wcore(kEncr, 1u << 31 | 5u << 16);
    CHECK(f.core(kKey0) == 1);
    CHECK(f.core(kKey0 + 12) == 4);
    CHECK(f.core(kAddrLo) == 0x44332211);
    CHECK(f.core(kAddrHi) == 0x6655);
    CHECK(f.core(kEncr) == (5u << 16 | 0x123));
    // Search: entry 5 was inserted before entry 3.
    f.wcore(kEncr, 1u << 29);
    CHECK(f.core(kEncr) == (5u << 16));
    // Not found.
    f.wcore(kAddrLo, 0x1);
    f.wcore(kEncr, 1u << 29 | 9u << 16);
    CHECK(f.core(kEncr) == (1u << 28 | 9u << 16));
    // Write has priority over read and search.
    set_key(0x1, 0x2, 7);
    f.wcore(kEncr, 1u << 30 | 1u << 31 | 1u << 29 | 8u << 16 | 0x42);
    set_key(0, 0, 0);
    f.wcore(kEncr, 1u << 31 | 8u << 16);
    CHECK(f.core(kAddrLo) == 1);
    CHECK(f.core(kKey0) == 7);
    // Reading a missing entry leaves the registers alone.
    f.wcore(kAddrLo, 0xAB);
    f.wcore(kEncr, 1u << 31 | 500u << 16);
    CHECK(f.core(kAddrLo) == 0xAB);
}

TEST_CASE("mac key storage keeps the first insertion position when an index is rewritten") {
    Fixture f;
    f.wcore(kAddrLo, 0x11);
    f.wcore(kEncr, 1u << 30 | 1u << 16);
    f.wcore(kEncr, 1u << 30 | 2u << 16);
    f.wcore(kEncr, 1u << 30 | 1u << 16 | 7); // entry 1 rewritten, still first
    f.wcore(kEncr, 1u << 29);
    CHECK(f.core(kEncr) == (1u << 16));
}

TEST_CASE("mac station key index finds the transmitter inside the key range") {
    Fixture                 f;
    std::array<uint8_t, 24> frame{};
    for (int i = 0; i < 6; ++i) frame[10 + i] = static_cast<uint8_t>(0x10 + i);
    auto store = [&](uint32_t index) {
        f.wcore(kAddrLo, 0x13121110);
        f.wcore(kAddrHi, 0x1514);
        f.wcore(kEncr, 1u << 30 | index << 16);
    };
    CHECK_FALSE(f.mac.station_key_index(frame).has_value());
    store(5);  // outside the default range 24..63
    store(70); // outside
    CHECK_FALSE(f.mac.station_key_index(frame).has_value());
    store(40);
    store(30);
    CHECK(f.mac.station_key_index(frame) == std::optional<uint32_t>(40)); // insertion order
    f.wcore(kKeyRange, 0x0A05);                                           // 5..10
    CHECK(f.mac.station_key_index(frame) == std::optional<uint32_t>(5));
    f.wcore(kKeyRange, 0x050A); // end < start: the default range
    CHECK(f.mac.station_key_index(frame) == std::optional<uint32_t>(40));
    f.wcore(kKeyRange, 0);
    CHECK(f.mac.station_key_index(frame) == std::optional<uint32_t>(40));
    frame[10] = 0;
    CHECK_FALSE(f.mac.station_key_index(frame).has_value());
    CHECK_FALSE(f.mac.station_key_index(std::span<const uint8_t>(frame.data(), 15)).has_value());
}

TEST_CASE("mac time on air for legacy and HT rates") {
    Fixture f;
    auto    air = [&](uint32_t length, uint32_t format, uint32_t mcs) {
        f.wpl(kAir1, length | format << 24);
        f.wpl(kAir2, mcs);
        f.wpl(kAirValue, 1u << 31);
        return f.pl(kAirValue);
    };
    const uint32_t valid = 1u << 30;
    CHECK(air(100, 0, 0) == (valid | 992));     // 1 Mbit/s: 192 + 800
    CHECK(air(100, 0, 2) == (valid | 337));     // 5.5 Mbit/s: 192 + 145.45
    CHECK(air(100, 0, 3) == (valid | 92));      // 11 Mbit/s: 20 + 72.7
    CHECK(air(100, 1, 4) == (valid | 153));     // 6 Mbit/s: 20 + 133.3
    CHECK(air(100, 0, 99) == (valid | 34));     // clamped to 54 Mbit/s: 20 + 14.8
    CHECK(air(100, 2, 7) == (valid | 52));      // HT 65 Mbit/s: 40 + 12.3
    CHECK(air(100, 3, 0) == (valid | 163));     // HT 6.5 Mbit/s: 40 + 123.07
    CHECK(air(100, 2, 15) == (valid | 52));     // clamped to MCS 7
    CHECK(air(0xFFFFF, 0, 0) == (valid | 184)); // 8388792 & 0xFFFF
    // A write without bit 31 is plain storage.
    f.wpl(kAirValue, 0x1234);
    CHECK(f.pl(kAirValue) == 0x1234);
}

TEST_CASE("mac_cntrl_2 soft reset bit clears itself at the next tick") {
    Fixture f;
    f.wpl(kMacCntrl2, 0x101);
    CHECK(f.pl(kMacCntrl2) == 0x101);
    f.mac.tick(10);
    CHECK(f.pl(kMacCntrl2) == 0x100);
    // Writing 0 before the tick cancels it.
    f.wpl(kMacCntrl2, 1);
    f.wpl(kMacCntrl2, 0);
    f.mac.tick(30);
    CHECK(f.pl(kMacCntrl2) == 0);
}

TEST_CASE("mac reset clears state, interrupts, timers, TSF offset, keys and the line cache") {
    Fixture f;
    f.at_us(0);
    f.wpl(kGenEnable, kMaster | kTimersBit | kIdleBit);
    f.wpl(kTxEnable, 0xFFFFFFFF);
    f.wcore(kState, 0x30);
    f.wcore(kState, 0);
    f.wpl(kUnmask, 1);
    f.wcore(kAbs0, 100);
    f.wpl(kEventSet, 1);
    f.wpl(kTsfLo, 5000);
    f.wcore(kAddrLo, 0x11);
    f.wcore(kEncr, 1u << 30 | 30u << 16);
    f.mac.raise_txrx(1);
    CHECK(f.level[Fixture::General]);
    CHECK(f.level[Fixture::Prot]);
    const int before = f.writes[Fixture::General];
    f.mac.reset();
    CHECK(f.core(kState) == 0);
    CHECK(f.pl(kEventSet) == 0);
    CHECK(f.pl(kGenStatus) == 0);
    CHECK(f.pl(kTxStatus) == 0);
    CHECK(f.core(0x004) == 0x00001C03);
    CHECK(f.pl(kTsfLo) == f.core(kLo));
    CHECK_FALSE(f.mac.next_event_ns().has_value());
    CHECK_FALSE(f.level[Fixture::General]);
    CHECK_FALSE(f.level[Fixture::Prot]);
    CHECK(f.writes[Fixture::General] == before + 1); // every line is recomputed after a reset
    std::array<uint8_t, 16> frame{};
    frame[10] = 0x11;
    CHECK_FALSE(f.mac.station_key_index(frame).has_value());
    f.wcore(kEncr, 1u << 29);
    CHECK(f.core(kEncr) == (1u << 28));
}
