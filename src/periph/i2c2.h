#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"
#include "i2c/device.h"

namespace bk {

// The master role of the I2C2 controller: three words, `config` (bus timing,
// clock divider, enable), `status` (bus commands and status) and `data`. Like
// I2c1 it handles one byte at a time, without a FIFO (the FIFO flags read as an
// empty receive and an empty transmit FIFO), and every bus phase takes nine bit
// times of the bus clock. When a phase ends the controller sets `int_flag` and
// waits until software clears it; the `status` word is written as a whole.
//
// What the drivers (the custom SDK's and the factory firmware's) rely on, and
// this model does:
//  - A write that sets `start` (it was 0) begins the address phase, with the
//    address byte taken from `data`. Hardware does not clear `start`: it stays 1
//    in `status` until software writes 0, and that is how the interrupt handler
//    tells the address phase from a data phase.
//  - `stop` is a one-shot command that reads back 0, as in I2c1 (the same
//    deliberate difference from the documented hardware, for the same reason):
//    the drivers write `status` back with their changes in every interrupt, and a
//    latched `stop` would end the next transfer in its address phase.
//  - A phase ends with `int_flag` and `ack` set to the answer of the slave
//    (1 = acknowledged). Software clears `int_flag` to go on: the next byte
//    is sent when the address byte had the write direction, received when it
//    had the read direction and `ack` (the ACK for the byte before it, or the
//    go-ahead for the first one) is set. A write with `stop` set ends the
//    transfer instead, and a clear `ack` in it is the NACK of the last byte.
//
// Not modelled: slave mode, arbitration loss, SCL timeout (their flags read 0),
// the idle and SCL timing fields, `clk_src` and the interrupt mode. `tx_mode`
// and `master` are derived, not stored.
class I2c2 : public Module, public I2cBus {
  public:
    // `irq` may be empty; `ref_hz` is the clock the bus divider divides.
    I2c2(std::string name, const Clock &clock, Line irq, uint32_t ref_hz);

    void attach(I2cDevice *device) override;

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 12}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;

  private:
    enum class Phase { Idle, Addressed, Data };
    enum class Kind { Address, Send, Receive, Stop };
    struct Event {
        uint64_t due_ns;
        Kind     kind;
    };

    void     write_config(uint32_t value);
    void     write_status(uint32_t value);
    uint32_t status() const;
    uint64_t byte_ns() const;
    void     schedule(Kind kind, uint64_t duration_ns);
    void     start();
    void     stop();
    void     abort();
    void     proceed();
    void     finish(Kind kind);
    void     drive();

    std::string                    name_;
    const Clock                   &clock_;
    Line                           irq_;
    uint32_t                       ref_hz_;
    std::map<uint8_t, I2cDevice *> devices_;

    uint32_t             config_   = 0;
    uint32_t             status_   = 0; // the writable fields and `ack`
    uint32_t             data_     = 0;
    bool                 transmit_ = false, ack_req_ = false, busy_ = false;
    Phase                phase_  = Phase::Idle;
    I2cDevice           *device_ = nullptr;
    std::optional<Event> event_;
    bool                 level_ = false;
};

} // namespace bk
