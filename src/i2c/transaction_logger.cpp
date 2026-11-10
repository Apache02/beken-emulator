#include "i2c/transaction_logger.h"

#include <cstdio>

namespace bk {

namespace {

std::string byte_text(uint8_t byte, bool ack) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%02X %s", byte, ack ? "ACK" : "NACK");
    return buf;
}

} // namespace

void I2cTransactionLogger::address(uint8_t address, bool read, bool ack) {
    char buf[32];
    std::snprintf(buf,
                  sizeof buf,
                  "%s 0x%02X %c %s",
                  line_.empty() ? "START" : "RESTART",
                  address,
                  read ? 'R' : 'W',
                  ack ? "ACK" : "NACK");
    event(Last::None, buf, "");
}

void I2cTransactionLogger::write(uint8_t byte, bool ack) {
    const std::string text = byte_text(byte, ack);
    event(Last::Write, "write " + text, text);
}

void I2cTransactionLogger::received(uint8_t byte) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "received 0x%02X, waiting for the ACK or NACK", byte);
    log_.log(Level::Debug, bus_ + ": " + buf);
    received_ = byte;
}

void I2cTransactionLogger::answer(bool ack) {
    if (!received_) return;
    const std::string text = byte_text(*received_, ack);
    received_.reset();
    event(Last::Read, "read " + text, text);
}

void I2cTransactionLogger::stop() {
    log_.log(Level::Debug, bus_ + ": STOP");
    flush("STOP");
}

void I2cTransactionLogger::abort() {
    received_.reset();
    if (line_.empty()) return;
    log_.log(Level::Debug, bus_ + ": transfer aborted");
    flush("aborted");
}

void I2cTransactionLogger::clear() {
    line_.clear();
    last_ = Last::None;
    received_.reset();
}

// `text` is the event on its own; `more` is what it adds to a run of bytes in
// the same direction.
void I2cTransactionLogger::event(Last kind, const std::string &text, const std::string &more) {
    log_.log(Level::Debug, bus_ + ": " + text);
    if (kind != Last::None && kind == last_)
        line_ += " " + more;
    else
        line_ += (line_.empty() ? "" : ", ") + text;
    last_ = kind;
}

void I2cTransactionLogger::flush(const std::string &end) {
    log_.log(Level::Info, bus_ + ": " + (line_.empty() ? end : line_ + ", " + end));
    clear();
}

} // namespace bk
