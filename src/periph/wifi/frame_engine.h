#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "core/module.h"

namespace bk {

// Moves frames between guest memory (transmit and receive descriptor chains)
// and the air. Every descriptor access goes through the bus master and any
// BusError is tolerated: a descriptor that points nowhere ends the walk, a
// failed write is logged.
class WifiFrameEngine {
  public:
    static constexpr uint32_t kQueues = 5; // 0..3 access categories, 4 beacon

    using Transmit = std::function<void(std::span<const uint8_t>)>;
    using Raise    = std::function<void(uint32_t)>;

    // `transmit` receives each frame at once; `raise` sets TX/RX interrupt bits.
    WifiFrameEngine(BusMemory &memory, Transmit transmit, Raise raise);

    // Sends the chain that starts at `head`, or, if `head` is 0, the one that
    // continues after the last descriptor sent on `queue`.
    void transmit(uint32_t queue, uint32_t head, uint64_t now_ns);
    // Loads the receive heads (either may be absent), forgets the last used
    // descriptors and delivers what is queued.
    void set_rx_heads(std::optional<uint32_t> header, std::optional<uint32_t> payload);
    // Queues a received frame (the newest 64 are kept) and delivers.
    void receive(std::span<const uint8_t> frame, uint64_t tsf, int rssi, std::optional<uint32_t> key);
    // Completes the transmissions that are due.
    void                    tick(uint64_t now_ns);
    std::optional<uint64_t> next_event_ns() const;
    void                    reset();

    static constexpr uint64_t kAirTimeUs = 150;

  private:
    struct Completion {
        uint64_t due_ns;
        uint32_t queue, thd;
    };
    struct Received {
        std::vector<uint8_t>    frame;
        uint64_t                tsf;
        int                     rssi;
        std::optional<uint32_t> key;
    };

    std::optional<std::vector<uint8_t>> read(uint32_t addr, uint32_t size) const;
    std::optional<uint32_t>             read_word(uint32_t addr) const;
    void                                write(uint32_t addr, std::span<const uint8_t> data) const;
    void                                write_word(uint32_t addr, uint32_t value) const;

    void flush();
    bool deliver(const Received &rx);

    BusMemory &memory_;
    Transmit   transmit_;
    Raise      raise_;

    std::array<uint32_t, kQueues> last_thd_{};
    std::vector<Completion>       completions_;

    uint32_t             header_head_ = 0, payload_head_ = 0;
    uint32_t             last_rhd_ = 0, last_rbd_ = 0;
    std::deque<Received> queue_;
};

} // namespace bk
