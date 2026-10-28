#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"
#include "i2c/device.h"

namespace bk {

// I2C master that handles one byte at a time, without a FIFO. Every bus phase
// (the address byte, a data byte) takes nine bit times of the bus clock; when
// it ends the controller sets `int_flag` and waits until software clears it.
//
// One deliberate difference from the documented hardware: `stop` is a one-shot
// command and reads back 0 once the STOP has been issued. The driver sets it in
// its working copy of the register and writes that copy back for the rest of
// the transfer, and a latched `stop` would turn the next transfer's START into a
// STOP.
class I2c1 : public Module, public I2cBus {
  public:
    // `irq` may be empty; `ref_hz` is the clock the bus divider divides.
    I2c1(std::string name, const Clock &clock, Line irq, uint32_t ref_hz);

    // Non-owning: the device must outlive this module. Throws
    // std::invalid_argument when the address is taken already.
    void attach(I2cDevice *device) override;

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 8}
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

    uint32_t             config_ = 0; // the writable fields
    uint32_t             data_   = 0;
    bool                 rx_ack_ = false, ack_req_ = false, busy_ = false;
    Phase                phase_  = Phase::Idle;
    I2cDevice           *device_ = nullptr;
    std::optional<Event> event_;
    bool                 level_ = false;
};

} // namespace bk
