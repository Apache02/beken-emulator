#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "core/endpoint.h"
#include "core/module.h"

namespace bk {

// Byte-oriented UART: TX goes straight to the endpoint, RX is a 128-byte FIFO
// fed from the endpoint. No baud rate, parity or framing.
class Uart : public Module {
  public:
    Uart(std::string name, const Clock &clock, Line irq);

    // Non-owning; nullptr detaches.
    void attach(ByteEndpoint *endpoint) { endpoint_ = endpoint; }

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x20}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override { return stop_deadline_; }
    void                    reset() override;

  private:
    static constexpr size_t kDepth = 128;

    uint32_t raw() const;
    uint32_t fifo_status() const;
    void     refill();
    void     update_line();

    std::string             name_;
    const Clock            &clock_;
    Line                    irq_;
    ByteEndpoint           *endpoint_ = nullptr;
    std::deque<uint8_t>     fifo_, backlog_;
    uint32_t                fifo_config_ = 0, irq_enable_ = 0, latched_ = 0;
    std::array<uint32_t, 8> words_{};
    uint64_t                last_tick_ = 0;
    std::optional<uint64_t> stop_deadline_;
};

} // namespace bk
