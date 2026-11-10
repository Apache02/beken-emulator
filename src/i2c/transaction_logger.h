#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "core/log.h"

namespace bk {

// Logs what an I2C master does on its bus as transactions. Every event goes to
// the logger at debug as it happens; at info a transaction is one line, written
// when it ends, from the START to the STOP:
//
//   I2C1: START 0x30 W ACK, write 0x04 ACK 0x05 ACK, RESTART 0x30 R ACK, read 0x5A ACK 0x5B NACK, STOP
//
// Bytes in a row in one direction share one "write" or "read". A received byte
// is held until the master answers it, since that answer is its ACK or NACK.
class I2cTransactionLogger {
  public:
    I2cTransactionLogger(Logger &log, std::string bus) : log_(log), bus_(std::move(bus)) {}

    // The address phase ended. A START inside an open transaction is a RESTART.
    void address(uint8_t address, bool read, bool ack);
    // A byte went out; `ack` is the slave's answer.
    void write(uint8_t byte, bool ack);
    // A byte came in; answer() completes it.
    void received(uint8_t byte);
    // The master answered the received byte, if there is one.
    void answer(bool ack);
    // The STOP: the transaction is logged.
    void stop();
    // The transfer was cut off without a STOP: what there is of it is logged.
    void abort();
    // Forgets everything, logging nothing.
    void clear();

  private:
    enum class Last { None, Write, Read };
    void event(Last kind, const std::string &text, const std::string &more);
    void flush(const std::string &end);

    Logger                &log_;
    std::string            bus_;
    std::string            line_;
    Last                   last_ = Last::None;
    std::optional<uint8_t> received_;
};

} // namespace bk
