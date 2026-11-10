#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "core/log.h"
#include "i2c/transaction_logger.h"

using namespace bk;

namespace {

struct Fixture {
    std::ostringstream   out;
    I2cTransactionLogger log{Logger::get("i2c-test"), "I2C1"};

    explicit Fixture(Level level = Level::Info) {
        Logger::reset_for_tests();
        Logger::configure(out, false);
        Logger::set_level("i2c-test", level);
    }
    ~Fixture() { Logger::reset_for_tests(); }
};

} // namespace

TEST_CASE("i2c transaction logger writes one info line from START to STOP") {
    Fixture f;
    f.log.address(0x30, false, true);
    f.log.write(0x04, true);
    f.log.write(0x05, false);
    CHECK(f.out.str().empty()); // nothing until the transaction ends
    f.log.stop();
    CHECK(f.out.str() == "[I] emulator.i2c-test: I2C1: START 0x30 W ACK, write 0x04 ACK 0x05 NACK, STOP\n");
}

TEST_CASE("i2c transaction logger: a write, a repeated START and a read in one line") {
    Fixture f;
    f.log.address(0x30, false, true);
    f.log.write(0x10, true);
    f.log.address(0x30, true, true);
    f.log.received(0x5A);
    f.log.answer(true);
    f.log.received(0x5B);
    f.log.answer(false);
    f.log.stop();
    CHECK(f.out.str() == "[I] emulator.i2c-test: I2C1: START 0x30 W ACK, write 0x10 ACK, RESTART 0x30 R ACK, "
                         "read 0x5A ACK 0x5B NACK, STOP\n");
}

TEST_CASE("i2c transaction logger: the next transaction starts with START again") {
    Fixture f;
    f.log.address(0x30, false, false);
    f.log.stop();
    f.log.address(0x31, true, true);
    f.log.stop();
    CHECK(f.out.str() == "[I] emulator.i2c-test: I2C1: START 0x30 W NACK, STOP\n"
                         "[I] emulator.i2c-test: I2C1: START 0x31 R ACK, STOP\n");
}

TEST_CASE("i2c transaction logger: an answer without a received byte adds nothing") {
    Fixture f;
    f.log.address(0x30, true, true);
    f.log.answer(true);
    f.log.stop();
    CHECK(f.out.str() == "[I] emulator.i2c-test: I2C1: START 0x30 R ACK, STOP\n");
}

TEST_CASE("i2c transaction logger: an abort logs what there is, a clear logs nothing") {
    Fixture f;
    f.log.abort(); // nothing open
    CHECK(f.out.str().empty());
    f.log.address(0x30, true, true);
    f.log.received(0x5A); // never answered
    f.log.abort();
    CHECK(f.out.str() == "[I] emulator.i2c-test: I2C1: START 0x30 R ACK, aborted\n");
    f.out.str("");
    f.log.address(0x30, false, true);
    f.log.clear();
    f.log.address(0x30, false, true); // a START again, not a RESTART
    f.log.stop();
    CHECK(f.out.str() == "[I] emulator.i2c-test: I2C1: START 0x30 W ACK, STOP\n");
}

TEST_CASE("i2c transaction logger logs every event at debug") {
    Fixture f(Level::Debug);
    f.log.address(0x30, true, true);
    f.log.received(0x5A);
    f.log.answer(false);
    f.log.stop();
    CHECK(f.out.str() == "[D] emulator.i2c-test: I2C1: START 0x30 R ACK\n"
                         "[D] emulator.i2c-test: I2C1: received 0x5A, waiting for the ACK or NACK\n"
                         "[D] emulator.i2c-test: I2C1: read 0x5A NACK\n"
                         "[D] emulator.i2c-test: I2C1: STOP\n"
                         "[I] emulator.i2c-test: I2C1: START 0x30 R ACK, read 0x5A NACK, STOP\n");
}
