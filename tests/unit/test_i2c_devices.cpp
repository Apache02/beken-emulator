#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "i2c/catalog.h"
#include "i2c/hi704.h"
#include "i2c/register_device.h"

using namespace bk;

namespace {

// Writes `bytes` as one transaction: the address matched, then each byte.
void write_to(I2cDevice &d, std::initializer_list<uint8_t> bytes) {
    REQUIRE(d.begin(false));
    for (uint8_t b : bytes) CHECK(d.write(b));
    d.stop();
}

std::string error_of(const std::string &spec) {
    try {
        parse_device_spec(spec);
    } catch (const std::invalid_argument &e) {
        return e.what();
    }
    return "(no error)";
}

} // namespace

TEST_CASE("a plain device acknowledges everything and reads 0xFF") {
    struct Plain : I2cDevice {
        Plain() : I2cDevice(0x20) {}
        std::string type() const override { return "plain"; }
    } d;
    CHECK(d.address() == 0x20);
    CHECK(d.begin(true));
    CHECK(d.write(0x12));
    CHECK(d.read() == 0xFF);
    d.stop();
}

TEST_CASE("register device: first byte sets the offset, the rest are stored and the offset advances") {
    RegisterDevice d(0x30);
    write_to(d, {0x10, 0xAA, 0xBB});
    write_to(d, {0x10});
    REQUIRE(d.begin(true));
    CHECK(d.read() == 0xAA);
    CHECK(d.read() == 0xBB);
    CHECK(d.read() == 0x00);
    d.stop();
}

TEST_CASE("register device: the offset survives a stop and wraps at 256") {
    RegisterDevice d(0x30);
    write_to(d, {0xFF, 0x11, 0x22});
    write_to(d, {0xFF});
    d.begin(true);
    CHECK(d.read() == 0x11);
    CHECK(d.read() == 0x22); // wrapped to offset 0
    d.stop();
    // A read transaction without a new offset continues where the last one ended.
    d.begin(true);
    CHECK(d.read() == 0x00);
    d.stop();
    write_to(d, {0x00});
    d.begin(true);
    CHECK(d.read() == 0x22);
}

TEST_CASE("register device: read-only registers read their value and ignore writes") {
    RegisterDevice d(0x30,
                     {
                         {0x04, 0x96}
    });
    write_to(d, {0x03, 0x01, 0xEE, 0x02}); // writes to 0x03, 0x04 (ignored), 0x05
    write_to(d, {0x03});
    d.begin(true);
    CHECK(d.read() == 0x01);
    CHECK(d.read() == 0x96);
    CHECK(d.read() == 0x02);
}

TEST_CASE("register device: a read transaction does not expect an offset byte") {
    RegisterDevice d(0x30);
    write_to(d, {0x05, 0x77});
    d.begin(true); // read: clears "expect offset"
    d.stop();
    // The next write starts a new transaction that expects an offset again.
    write_to(d, {0x05});
    d.begin(true);
    CHECK(d.read() == 0x77);
}

TEST_CASE("hi704 has chip id 0x96 at register 4 and address 0x30 by default") {
    Hi704 d;
    CHECK(d.address() == 0x30);
    CHECK(d.type() == "hi704");
    write_to(d, {0x04});
    d.begin(true);
    CHECK(d.read() == 0x96);
    Hi704 other(0x31);
    CHECK(other.address() == 0x31);
}

TEST_CASE("device spec: type alone, case-insensitive, with an address") {
    auto d = parse_device_spec("hi704");
    CHECK(d->type() == "hi704");
    CHECK(d->address() == 0x30);
    CHECK(parse_device_spec("HI704")->address() == 0x30);
    CHECK(parse_device_spec("Hi704@0x31")->address() == 0x31);
    CHECK(parse_device_spec("hi704@49")->address() == 49);
    CHECK(parse_device_spec("hi704@0x08")->address() == 0x08);
    CHECK(parse_device_spec("hi704@0x77")->address() == 0x77);
}

TEST_CASE("device spec errors") {
    CHECK(error_of("ov7670") == "unknown I2C device 'ov7670' (known: hi704)");
    CHECK(error_of("ov7670@0x30") == "unknown I2C device 'ov7670' (known: hi704)");
    const std::string want = " (want TYPE[@ADDRESS], ADDRESS a 7-bit number 0x08-0x77)";
    for (const char *bad : {"hi704@",
                            "hi704@0x07",
                            "hi704@0x78",
                            "hi704@x",
                            "hi704@0x30z",
                            "hi704@-1",
                            "hi704@ 0x30",
                            "hi704@010",
                            "hi704@0x"})
        CHECK_MESSAGE(error_of(bad) == "bad I2C device '" + std::string(bad) + "'" + want, bad);
}
