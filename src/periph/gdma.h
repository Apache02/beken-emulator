#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// General DMA with six channels. A channel moves `transfer_length + 1` bytes
// through a byte accumulator: each source read fetches a 32-bit word and keeps
// its low 2^src_width bytes, each destination write drains 2^dst_width bytes
// into the low lanes of a 32-bit word. Addresses step by 4 per access when
// incrementing and wrap inside a loop window when one is enabled (the end is
// exclusive). An under-filled last write is zero padded.
//
// Memory-to-memory channels move their words as device time passes, so a
// stopped transfer leaves what it wrote so far, and `remain_len` (in bytes)
// and `dst_wr_addr` keep their values after a stop. Every enable 0 -> 1 starts
// over from the start addresses. Repeat mode starts the next cycle where the
// last one ended. The running channels share the bus: round-robin splits it
// evenly, fixed priority serves the higher priority (then the higher channel
// number) first and never grants a channel of priority 0.
//
// Within one tick a channel stops at the first event whose flag is enabled
// (half finish, finish), so a firmware polling the flag sees `remain_len` at
// that point. Channels whose source is a peripheral with a request line move
// a word whenever the peripheral holds its line up. Any other request line is
// reported and not emulated (the channel stalls).
class Gdma : public Module {
  public:
    static constexpr unsigned kChannels = 6;

    // `irq` may be empty; `bus_hz` is the clock the transfer times were
    // measured at.
    Gdma(std::string name, const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x200}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    void                    reset() override;
    std::shared_ptr<RequestLine> request_line(unsigned n) override;

  private:
    // [start, end) of a loop window.
    struct Window {
        uint32_t start, end;
    };

    // One cycle of a channel's data movement; it can stop in the middle when
    // a request line source has no word and carry on later.
    class Mover {
      public:
        Mover(BusMemory                   &memory,
              uint64_t                     src,
              uint64_t                     dst,
              uint32_t                     length,
              unsigned                     src_width,
              unsigned                     dst_width,
              bool                         src_inc,
              bool                         dst_inc,
              std::optional<Window>        src_window = {},
              std::optional<Window>        dst_window = {},
              std::shared_ptr<RequestLine> port       = {});

        bool done() const { return writes >= writes_total; }
        // Does up to `max_writes` destination writes (all when empty) and
        // returns how many were done. Throws BusError.
        uint64_t run(std::optional<uint64_t> max_writes);
        // Bytes of the cycle not written yet.
        uint32_t remain() const;
        // Destination writes after which half of the bytes are written.
        uint64_t half_writes() const;

        uint64_t src, dst;
        uint64_t writes_total, reads_total, writes = 0, reads = 0;

      private:
        BusMemory                   &memory_;
        std::shared_ptr<RequestLine> port_;
        uint32_t                     length_;
        uint64_t                     src_bytes_, dst_bytes_;
        bool                         src_inc_, dst_inc_;
        std::optional<Window>        src_window_, dst_window_;
        uint32_t                     taken_ = 0;
        std::vector<uint8_t>         accumulator_;
    };

    struct Channel {
        uint32_t                config = 0, dst = 0, src = 0, mux = 0;
        std::array<uint32_t, 4> loops{};
        std::array<uint32_t, 2> pause{};
        bool                    running = false, stalled = false;
        bool                    stuck = false; // fixed priority 0: never granted until enable is cleared
        std::unique_ptr<Mover>  mover;         // kept after a stop or a finish: remain_len and dst_wr_addr
        // Bus transactions (the larger of reads and writes) of the cycle, how
        // far the cycle got, where its half-finish event is and the cost of one
        // transaction in tenths of a bus cycle when the channel has the bus alone.
        uint64_t                     transactions = 0, half_at = 0, period = 0;
        double                       progress     = 0;
        bool                         half_pending = false;
        uint32_t                     fin_count = 0, half_count = 0;
        bool                         paced = false; // a request line decides when words move
        std::shared_ptr<RequestLine> port;
        uint64_t                     word_ns = 0, last_ns = 0;
    };

    uint32_t                      read_channel(const Channel &channel, uint32_t within) const;
    void                          write_channel(unsigned index, uint32_t within, uint32_t value);
    void                          write_config(unsigned index, Channel &channel, uint32_t value);
    void                          start(unsigned index, Channel &channel);
    void                          begin_cycle(Channel &channel, uint64_t src, uint64_t dst);
    void                          advance(uint64_t now_ns);
    std::array<double, kChannels> rates(const std::array<bool, kChannels> &held) const;
    // Moves the words `progress` allows; false when the channel was aborted.
    bool move(unsigned index, Channel &channel);
    // Handles the event at the channel's target; true when its flag is enabled.
    bool     reach_target(unsigned index, Channel &channel);
    double   target(const Channel &channel) const;
    bool     has_event(const Channel &channel) const;
    void     run_paced(unsigned index, Channel &channel, uint64_t now_ns);
    void     half_finished(unsigned index, Channel &channel);
    void     end_cycle(unsigned index, Channel &channel);
    void     flag_finished(unsigned index, Channel &channel);
    void     abort(unsigned index, Channel &channel, const BusError &err);
    void     acknowledge(uint32_t value);
    uint32_t status(unsigned index) const;
    uint32_t dst_wr_addr(unsigned index) const;
    void     update_irq();

    std::string                                      name_;
    const Clock                                     &clock_;
    Line                                             irq_;
    BusMemory                                       &memory_;
    uint32_t                                         bus_hz_;
    std::array<Channel, kChannels>                   channels_;
    std::map<unsigned, std::shared_ptr<RequestLine>> ports_;
    uint32_t                                         fin_ = 0, half_ = 0, prio_ = 0;
    uint64_t                                         advanced_ns_ = 0; // device time the channels were moved to
};

} // namespace bk
