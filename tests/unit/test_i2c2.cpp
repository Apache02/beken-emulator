#include <doctest/doctest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/log.h"
#include "periph/i2c2.h"

using namespace bk;

namespace {

constexpr uint32_t kConfig = 0, kStatus = 4, kData = 8;
constexpr uint32_t kEnable  = 1u << 31;
constexpr uint32_t kIntFlag = 1u << 0, kRxFifoEmpty = 1u << 4, kAck = 1u << 8, kStop = 1u << 9, kStart = 1u << 10,
                   kAckReq = 1u << 12, kTxMode = 1u << 13, kMaster = 1u << 14, kBusy = 1u << 15;

struct FakeClock : Clock {
    uint64_t now = 0;
    uint64_t now_ns() const override { return now; }
};

// Records what the controller does to it.
struct Recorder : I2cDevice {
    std::vector<std::string> log;
    bool                     ack  = true;
    uint8_t                  next = 0x5A;
    Recorder(uint8_t address, bool ack_all = true) : I2cDevice(address), ack(ack_all) {}
    std::string type() const override { return "recorder"; }
    bool        begin(bool read) override {
        log.push_back(read ? "begin-read" : "begin-write");
        return ack;
    }
    bool write(uint8_t b) override {
        log.push_back("write " + std::to_string(b));
        return ack;
    }
    uint8_t read() override {
        log.push_back("read");
        return next++;
    }
    void stop() override { log.push_back("stop"); }
};

struct Fixture {
    FakeClock         clock;
    std::vector<bool> irq;
    I2c2              i2c;
    Recorder          dev{0x30};

    explicit Fixture(uint32_t ref_hz = 26'000'000) : i2c("I2C2", clock, [this](bool v) { irq.push_back(v); }, ref_hz) {
        i2c.attach(&dev);
        w(kConfig, kEnable);
    }

    void     w(uint32_t off, uint32_t v) { i2c.write("regs", off, 4, v); }
    uint32_t r(uint32_t off) { return i2c.read("regs", off, 4); }
    bool     line() const { return !irq.empty() && irq.back(); }
    uint64_t byte_ns(uint32_t divider = 0) const {
        return 9ULL * (6 + 3 * (divider + 1)) * 1'000'000'000ULL / 26'000'000;
    }
    // Advances to the pending event and lets the module handle it.
    void step() {
        REQUIRE(i2c.next_event_ns().has_value());
        clock.now = *i2c.next_event_ns();
        i2c.tick(clock.now);
    }
};

} // namespace

TEST_CASE("i2c2 has one range of 12 bytes and resets idle") {
    Fixture f;
    REQUIRE(f.i2c.ranges().size() == 1);
    CHECK(f.i2c.ranges()[0].name == "regs");
    CHECK(f.i2c.ranges()[0].size == 12);
    f.i2c.reset();
    CHECK(f.r(kConfig) == 0);
    CHECK(f.r(kData) == 0);
    // Only the FIFO flag of an empty receive FIFO is set.
    CHECK(f.r(kStatus) == kRxFifoEmpty);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c2 config keeps every bit and data keeps 8 bits") {
    Fixture f;
    f.w(kConfig, 0xFFFFFFFF);
    CHECK(f.r(kConfig) == 0xFFFFFFFF);
    f.w(kData, 0x1234);
    CHECK(f.r(kData) == 0x34);
}

TEST_CASE("i2c2 status keeps the writable bits and the others read as hardware has them") {
    Fixture f;
    f.w(kStatus, 0xFFFFFFFF & ~kStart & ~kStop);
    // int_flag, int_mode and ack are stored; the flags the hardware owns are not.
    CHECK(f.r(kStatus) == (kIntFlag | (3u << 6) | kAck | kRxFifoEmpty));
}

TEST_CASE("i2c2 start schedules the address byte one byte time later and stays set") {
    Fixture f;
    f.clock.now = 1000;
    f.w(kData, 0x60); // address 0x30, write
    f.w(kStatus, kStart);
    CHECK((f.r(kStatus) & (kMaster | kBusy)) == (kMaster | kBusy));
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(1000 + f.byte_ns()));
    CHECK_FALSE(f.line());
    f.step();
    CHECK(f.line());
    // The handler tells the address phase by `start` still being set.
    CHECK((f.r(kStatus) & (kIntFlag | kStart | kAck | kTxMode)) == (kIntFlag | kStart | kAck | kTxMode));
    CHECK(f.dev.log == std::vector<std::string>{"begin-write"});
}

TEST_CASE("i2c2 byte time follows the divider and the reference clock") {
    Fixture f;
    f.w(kConfig, kEnable | (10u << 6));
    f.w(kData, 0x60);
    f.w(kStatus, kStart);
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(f.byte_ns(10)));

    Fixture slow(1'000'000);
    slow.w(kStatus, kStart);
    CHECK(slow.i2c.next_event_ns() == std::optional<uint64_t>(9ULL * 9 * 1'000'000'000ULL / 1'000'000));

    // Never zero, even at an absurd clock.
    Fixture fast(0xFFFFFFFFu);
    fast.w(kStatus, kStart);
    CHECK(*fast.i2c.next_event_ns() >= 1);
}

TEST_CASE("i2c2 a write transaction as the custom SDK driver runs it") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kStatus, kStart);
    f.step();
    REQUIRE((f.r(kStatus) & kIntFlag) != 0);
    // Address phase: start off, int_flag off, first byte.
    f.w(kData, 0xA1);
    f.w(kStatus, kAck);
    CHECK_FALSE(f.line());
    f.step();
    CHECK(f.line());
    CHECK((f.r(kStatus) & kAck) != 0);
    f.w(kData, 0xA2);
    f.w(kStatus, kAck);
    f.step();
    // The last byte was acknowledged: stop.
    f.w(kStatus, kAck | kStop);
    CHECK_FALSE(f.line());
    CHECK((f.r(kStatus) & kBusy) != 0); // busy until the STOP is on the wire
    f.step();
    CHECK((f.r(kStatus) & kBusy) == 0);
    CHECK(f.dev.log == std::vector<std::string>{"begin-write", "write 161", "write 162", "stop"});
    // `stop` is one-shot: it reads back 0.
    CHECK((f.r(kStatus) & kStop) == 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c2 the factory driver writes the status word back with what it read") {
    Fixture f;
    // A transfer that ends with a stop, then the next one at once: the driver
    // sets `start` on what it reads, and the STOP has not completed yet.
    f.w(kData, 0x60);
    f.w(kStatus, kStart);
    f.step();
    f.w(kStatus, kAck | kStop);
    const uint32_t seen = f.r(kStatus);
    CHECK((seen & kStop) == 0);
    f.w(kData, 0x60);
    f.w(kStatus, seen | kStart);
    f.step();
    CHECK((f.r(kStatus) & kIntFlag) != 0);
    // The interrupt handler writes the status back as it read it, start cleared.
    f.w(kData, 0xA1);
    f.w(kStatus, f.r(kStatus) & ~kStart & ~kIntFlag);
    // The address phase is not cut short: the byte goes out.
    f.step();
    CHECK(f.dev.log == std::vector<std::string>{"begin-write", "stop", "begin-write", "write 161"});
    CHECK((f.r(kStatus) & kIntFlag) != 0);
}

TEST_CASE("i2c2 a read transaction ends with a NACK of the last byte and a stop") {
    Fixture f;
    f.w(kData, 0x61);
    f.w(kStatus, kStart);
    f.step();
    CHECK((f.r(kStatus) & kTxMode) == 0);
    // Address acknowledged: ack on, start off, int_flag off starts the first byte.
    f.w(kStatus, kAck);
    f.step();
    CHECK(f.r(kData) == 0x5A);
    CHECK((f.r(kStatus) & (kIntFlag | kAckReq)) == (kIntFlag | kAckReq));
    // More bytes wanted: ACK this one.
    f.w(kStatus, kAck);
    CHECK((f.r(kStatus) & kAckReq) == 0);
    f.step();
    CHECK(f.r(kData) == 0x5B);
    // The last one: NACK and stop in the same write.
    f.w(kStatus, kStop);
    f.step();
    CHECK(f.dev.log == std::vector<std::string>{"begin-read", "read", "read", "stop"});
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c2 a clear ack without a stop does not receive the next byte") {
    Fixture f;
    f.w(kData, 0x61);
    f.w(kStatus, kStart);
    f.step();
    f.w(kStatus, kAck);
    f.step();
    f.w(kStatus, 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c2 a NACK of the address") {
    Fixture  f;
    Recorder deaf(0x31, false);
    f.i2c.attach(&deaf);
    f.w(kData, 0x62); // address 0x31, write
    f.w(kStatus, kStart);
    f.step();
    CHECK((f.r(kStatus) & kAck) == 0);
    CHECK(deaf.log == std::vector<std::string>{"begin-write"});
    f.w(kStatus, kStop);
    f.step();
    // The device refused the address, so it is not told about the stop.
    CHECK(deaf.log == std::vector<std::string>{"begin-write"});

    // No device at the address: the same.
    f.w(kData, 0x6E);
    f.w(kStatus, kStart);
    f.step();
    CHECK((f.r(kStatus) & kAck) == 0);
}

TEST_CASE("i2c2 a second transfer starts again after a stop") {
    Fixture f;
    for (int i = 0; i < 2; ++i) {
        f.w(kData, 0x60);
        f.w(kStatus, kStart);
        f.step();
        f.w(kStatus, kStop);
        f.step();
    }
    CHECK(f.dev.log == std::vector<std::string>{"begin-write", "stop", "begin-write", "stop"});
}

TEST_CASE("i2c2 switching the engine off aborts the transfer and clears the flag") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kStatus, kStart);
    f.step();
    REQUIRE(f.line());
    f.w(kConfig, 0);
    CHECK_FALSE(f.line());
    CHECK((f.r(kStatus) & (kIntFlag | kBusy)) == 0);
    CHECK(f.dev.log == std::vector<std::string>{"begin-write", "stop"});
    // Off, a start does nothing.
    f.w(kStatus, kStart);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
    // On again, it works.
    f.w(kConfig, kEnable);
    f.w(kStatus, kStart);
    CHECK(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c2 rejects two devices at one address") {
    Fixture  f;
    Recorder second(0x30);
    CHECK_THROWS_WITH_AS(
        f.i2c.attach(&second), "I2C2 address 0x30 is used twice (recorder and recorder)", std::invalid_argument);
}

TEST_CASE("i2c2 reset clears registers, the event and the line") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kStatus, kStart);
    f.step();
    REQUIRE(f.line());
    f.i2c.reset();
    CHECK_FALSE(f.line());
    CHECK(f.r(kConfig) == 0);
    CHECK(f.r(kStatus) == kRxFifoEmpty);
    CHECK(f.r(kData) == 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c2 works without an interrupt line") {
    FakeClock clock;
    I2c2      i2c("I2C2", clock, {}, 26'000'000);
    i2c.write("regs", kConfig, 4, kEnable);
    i2c.write("regs", kStatus, 4, kStart);
    clock.now = *i2c.next_event_ns();
    i2c.tick(clock.now);
    CHECK((i2c.read("regs", kStatus, 4) & kIntFlag) != 0);
}
