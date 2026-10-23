#include <doctest/doctest.h>

#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/log.h"
#include "i2c/register_device.h"
#include "periph/i2c1.h"

using namespace bk;

namespace {

constexpr uint32_t kConfig = 0, kData = 4;
constexpr uint32_t kEnsmb = 1u << 0, kSta = 1u << 1, kSto = 1u << 2, kAckTx = 1u << 3, kTxMode = 1u << 4;
constexpr uint32_t kSi = 1u << 16, kAckRx = 1u << 17, kAckReq = 1u << 18, kBusy = 1u << 19;
constexpr uint32_t kDiv0 = 0; // divider 0: byte time = 9 * 9 / 26 MHz = 3115 ns

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
    I2c1              i2c;
    Recorder          dev{0x30};

    explicit Fixture(uint32_t ref_hz = 26'000'000) : i2c("I2C1", clock, [this](bool v) { irq.push_back(v); }, ref_hz) {
        i2c.attach(&dev);
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

TEST_CASE("i2c1 has one range of 8 bytes and resets idle") {
    Fixture f;
    REQUIRE(f.i2c.ranges().size() == 1);
    CHECK(f.i2c.ranges()[0].name == "regs");
    CHECK(f.i2c.ranges()[0].size == 8);
    CHECK(f.r(kConfig) == 0);
    CHECK(f.r(kData) == 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c1 data register keeps 8 bits") {
    Fixture f;
    f.w(kData, 0x1234);
    CHECK(f.r(kData) == 0x34);
}

TEST_CASE("i2c1 config keeps only the writable bits and reads sto back as 0") {
    Fixture f;
    f.w(kConfig, kEnsmb | kAckTx | kTxMode | (0x155u << 6));
    CHECK(f.r(kConfig) == (kEnsmb | kAckTx | kTxMode | (0x155u << 6)));
    f.w(kConfig, 0xFFFFFFFF & ~kSta & ~kSto); // status bits are not writable
    CHECK((f.r(kConfig) & (kAckRx | kAckReq | kBusy)) == 0);
    CHECK((f.r(kConfig) & kSto) == 0);
}

TEST_CASE("i2c1 start schedules the address byte one byte time later") {
    Fixture f;
    f.clock.now = 1000;
    f.w(kData, 0x60); // address 0x30, write
    f.w(kConfig, kEnsmb | kSta);
    CHECK((f.r(kConfig) & kBusy) != 0);
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(1000 + f.byte_ns()));
    CHECK_FALSE(f.line());
    f.step();
    CHECK(f.line());
    CHECK((f.r(kConfig) & kSi) != 0);
    CHECK((f.r(kConfig) & kAckRx) != 0);
    CHECK(f.dev.log == std::vector<std::string>{"begin-write"});
}

TEST_CASE("i2c1 byte time follows the divider and the reference clock") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta | (10u << 6));
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(f.byte_ns(10)));
    CHECK(f.byte_ns(10) == 9ULL * 39 * 1'000'000'000ULL / 26'000'000);

    Fixture slow(1'000'000);
    slow.w(kConfig, kEnsmb | kSta);
    CHECK(slow.i2c.next_event_ns() == std::optional<uint64_t>(9ULL * 9 * 1'000'000'000ULL / 1'000'000));

    // Never zero, even at an absurd clock.
    Fixture fast(0xFFFFFFFFu);
    fast.w(kConfig, kEnsmb | kSta);
    CHECK(*fast.i2c.next_event_ns() >= 1);
}

TEST_CASE("i2c1 a full write transaction") {
    Fixture f;
    // Address 0x30 write.
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    REQUIRE(f.line());
    // Software clears si and sends a byte.
    f.w(kData, 0x04);
    f.w(kConfig, kEnsmb | kTxMode); // sta stays clear, si clear
    CHECK_FALSE(f.line());
    f.step();
    CHECK(f.line());
    CHECK((f.r(kConfig) & kAckRx) != 0);
    // Stop.
    f.w(kConfig, kEnsmb | kTxMode | kSto);
    CHECK_FALSE(f.line());
    CHECK((f.r(kConfig) & kSto) == 0); // one-shot
    CHECK((f.r(kConfig) & kBusy) != 0);
    f.step();
    CHECK((f.r(kConfig) & kBusy) == 0);
    CHECK(f.dev.log == std::vector<std::string>{"begin-write", "write 4", "stop"});
}

TEST_CASE("i2c1 a nacked address forgets the device and nacks the data") {
    Fixture f;
    f.w(kData, 0x62); // address 0x31: nobody
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    CHECK((f.r(kConfig) & kAckRx) == 0);
    CHECK(f.line());
    f.w(kData, 0x01);
    f.w(kConfig, kEnsmb | kTxMode);
    f.step();
    CHECK((f.r(kConfig) & kAckRx) == 0);
    CHECK(f.dev.log.empty());
}

TEST_CASE("i2c1 a device that does not acknowledge the address is forgotten") {
    Fixture f;
    f.dev.ack = false;
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    CHECK((f.r(kConfig) & kAckRx) == 0);
    f.dev.ack = true;
    f.w(kData, 0x07);
    f.w(kConfig, kEnsmb | kTxMode);
    f.step();
    CHECK(f.dev.log == std::vector<std::string>{"begin-write"}); // the data byte never reached it
    CHECK((f.r(kConfig) & kAckRx) == 0);
}

TEST_CASE("i2c1 read: the first byte follows the address, the next ones need ack_tx") {
    Fixture f;
    f.w(kData, 0x61); // address 0x30, read
    f.w(kConfig, kEnsmb | kSta);
    f.step();
    REQUIRE(f.line());
    // Clear si: the phase is `addressed`, so a byte is received.
    f.w(kConfig, kEnsmb);
    CHECK_FALSE(f.line());
    f.step();
    CHECK(f.line());
    CHECK(f.r(kData) == 0x5A);
    CHECK((f.r(kConfig) & kAckReq) != 0);
    // Without ack_tx the next clear of si does not receive anything.
    f.w(kConfig, kEnsmb);
    CHECK((f.r(kConfig) & kAckReq) == 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c1 read with ack_tx keeps receiving") {
    Fixture f;
    f.w(kData, 0x61);
    f.w(kConfig, kEnsmb | kSta | kAckTx);
    f.step();
    f.w(kConfig, kEnsmb | kAckTx);
    f.step();
    CHECK(f.r(kData) == 0x5A);
    f.w(kConfig, kEnsmb | kAckTx);
    f.step();
    CHECK(f.r(kData) == 0x5B);
    CHECK(f.dev.log == std::vector<std::string>{"begin-read", "read", "read"});
}

TEST_CASE("i2c1 read without a device returns 0xFF") {
    Fixture f;
    f.w(kData, 0x63);
    f.w(kConfig, kEnsmb | kSta);
    f.step();
    f.w(kConfig, kEnsmb);
    f.step();
    CHECK(f.r(kData) == 0xFF);
}

TEST_CASE("i2c1 stop before any phase happened is ignored, stop in a phase tells the device") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta);
    f.w(kConfig, kEnsmb | kSto); // phase is still idle (the address byte is on the wire)
    CHECK(f.dev.log.empty());
    // The pending event is the address byte; sta was in `previous`, so it is not restarted.
    f.step();
    f.w(kConfig, kEnsmb | kSto);
    CHECK(f.dev.log.back() == "stop");
    CHECK((f.r(kConfig) & kSi) == 0);
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(f.clock.now + f.byte_ns() / 4));
}

TEST_CASE("i2c1 sta set again after being set does not restart") {
    Fixture f;
    f.w(kConfig, kEnsmb | kSta);
    uint64_t due = *f.i2c.next_event_ns();
    f.clock.now  = 100;
    f.w(kConfig, kEnsmb | kSta);
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(due));
    // Clearing and setting sta does restart (and the pending event is replaced).
    f.w(kConfig, kEnsmb);
    f.w(kConfig, kEnsmb | kSta);
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(100 + f.byte_ns()));
}

TEST_CASE("i2c1 clearing ensmb aborts the transfer") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    REQUIRE(f.line());
    f.w(kConfig, 0);
    CHECK_FALSE(f.line());
    CHECK(f.dev.log.back() == "stop");
    CHECK(f.r(kConfig) == 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c1 a new event replaces the pending one") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    f.w(kData, 1);
    f.w(kConfig, kEnsmb | kTxMode); // send scheduled
    REQUIRE(f.i2c.next_event_ns().has_value());
    f.w(kConfig, kEnsmb | kTxMode | kSto); // stop replaces it
    f.step();                              // only the stop event runs
    CHECK(f.dev.log == std::vector<std::string>{"begin-write", "stop"});
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c1 tick before the due time does nothing") {
    Fixture f;
    f.w(kConfig, kEnsmb | kSta);
    uint64_t due = *f.i2c.next_event_ns();
    f.i2c.tick(due - 1);
    CHECK_FALSE(f.line());
    CHECK(f.i2c.next_event_ns() == std::optional<uint64_t>(due));
}

TEST_CASE("i2c1 attaching two devices at one address is an error") {
    Fixture  f;
    Recorder second(0x30);
    CHECK_THROWS_WITH_AS(
        f.i2c.attach(&second), "I2C1 address 0x30 is used twice (recorder and recorder)", std::invalid_argument);
    Recorder other(0x31);
    CHECK_NOTHROW(f.i2c.attach(&other));
}

TEST_CASE("i2c1 reset clears registers, the event and the line") {
    Fixture f;
    f.w(kData, 0x60);
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    REQUIRE(f.line());
    f.i2c.reset();
    CHECK_FALSE(f.line());
    CHECK(f.r(kConfig) == 0);
    CHECK(f.r(kData) == 0);
    CHECK_FALSE(f.i2c.next_event_ns().has_value());
}

TEST_CASE("i2c1 works without an interrupt line") {
    FakeClock clock;
    I2c1      i2c("I2C1", clock, {}, 26'000'000);
    i2c.write("regs", kConfig, 4, kEnsmb | kSta);
    clock.now = *i2c.next_event_ns();
    i2c.tick(clock.now);
    CHECK((i2c.read("regs", kConfig, 4) & kSi) != 0);
}

TEST_CASE("i2c1 talks to a register device") {
    Fixture f;
    f.i2c.reset();
    // Replace the recorder with a real device at another address.
    RegisterDevice reg(0x40,
                       {
                           {0x04, 0x96}
    });
    f.i2c.attach(&reg);
    auto send = [&](uint8_t byte) {
        f.w(kData, byte);
        f.w(kConfig, kEnsmb | kTxMode);
        f.step();
    };
    f.w(kData, 0x80); // 0x40 write
    f.w(kConfig, kEnsmb | kSta | kTxMode);
    f.step();
    send(0x04); // offset
    f.w(kConfig, kEnsmb | kTxMode | kSto);
    f.step();
    f.w(kConfig, kEnsmb);
    f.w(kData, 0x81); // 0x40 read
    f.w(kConfig, kEnsmb | kSta);
    f.step();
    f.w(kConfig, kEnsmb);
    f.step();
    CHECK(f.r(kData) == 0x96);
}

TEST_CASE("i2c1 logs config writes, phases and the interrupt line at DEBUG") {
    std::ostringstream out;
    Logger::reset_for_tests();
    Logger::configure(out, false);
    Logger::set_debug("i2c");
    {
        Fixture f;
        f.w(kData, 0x60);
        f.w(kConfig, kEnsmb | kSta);
        f.step();
    }
    std::string text = out.str();
    Logger::reset_for_tests();
    CHECK(text.find("config <= 0x00000003 (ensmb sta div=0)") != std::string::npos);
    CHECK(text.find("address 0x30 write: ack") != std::string::npos);
    CHECK(text.find("interrupt line high") != std::string::npos);
}
