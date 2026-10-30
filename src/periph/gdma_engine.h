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

// The channels of a general DMA, shared by the GDMA modules, which differ
// only in their register layout. It holds no registers: a module stores the
// words of its layout here and reads the state back.
//
// A channel moves `transfer_length + 1` bytes (config bits 31:16) through a
// byte accumulator: each source read fetches a 32-bit word and keeps its low
// 2^src_width bytes, each destination write drains 2^dst_width bytes into the
// low lanes of a 32-bit word. Addresses step by 4 per access when incrementing
// and wrap inside a loop window when one is enabled (the end is exclusive). An
// under-filled last write is zero padded. The config word: enable (bit 0),
// finish and half-finish interrupt enables (1, 2), repeat (3), source and
// destination widths (5:4, 7:6), increments (8, 9), loops (10, 11), priority
// (from bit 12).
//
// Memory-to-memory channels move their words as device time passes, so a
// stopped transfer leaves what it wrote so far, and the remaining length (in
// bytes) and the write address keep their values after a stop. Every enable
// 0 -> 1 starts over from the start addresses. Repeat mode starts the next
// cycle where the last one ended. The running channels share the bus:
// round-robin splits it evenly, fixed priority serves the higher priority
// (then the higher channel number) first and never grants a channel of
// priority 0.
//
// Within one tick a channel stops at the first event whose flag is enabled
// (half finish, finish), so a firmware polling the flag sees the remaining
// length at that point. Channels whose source is a peripheral with a request
// line move a word whenever the peripheral holds its line up. Any other
// request line is reported and not emulated (the channel stalls). The
// interrupt line is up while a finish or half-finish flag is set.
class GdmaEngine {
  public:
    static constexpr unsigned kMaxChannels = 8;

    // How a layout encodes the request lines in a channel's mux word: the
    // source and destination fields, the line numbers that mean memory, the
    // width of the priority field in the config word and the names of the
    // lines for the log (nullptr when a line has none).
    struct Encoding {
        unsigned              src_shift, dst_shift, request_bits;
        std::vector<uint32_t> memory_requests;
        unsigned              priority_bits;
        const char *(*request_name)(uint32_t request);
    };

    // The plain words of a channel; config is apart, since writing it starts
    // and stops the channel. loops: destination end and start, source end and
    // start. pause: source, destination.
    struct Words {
        uint32_t                dst = 0, src = 0, mux = 0;
        std::array<uint32_t, 4> loops{};
        std::array<uint32_t, 2> pause{};
    };

    // `irq` may be empty; `bus_hz` is the clock the transfer times were
    // measured at.
    GdmaEngine(const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz, unsigned channels, Encoding encoding);

    unsigned channels() const { return count_; }

    // A module calls sync() before it reads or writes a register (it brings the
    // channels to the device time) and settle() after a write.
    void sync();
    void settle();

    // The config word as written, with bit 0 telling whether the channel runs.
    uint32_t config(unsigned channel) const;
    void     write_config(unsigned channel, uint32_t value);
    Words   &words(unsigned channel) { return channels_[channel].words; }

    // Bytes of the current cycle not written yet, the 4-bit counts of finish
    // and half-finish events, and where the next read and write go.
    uint32_t remain(unsigned channel) const;
    uint32_t fin_count(unsigned channel) const { return channels_[channel].fin_count; }
    uint32_t half_count(unsigned channel) const { return channels_[channel].half_count; }
    uint32_t src_rd_addr(unsigned channel) const;
    uint32_t dst_wr_addr(unsigned channel) const;

    // One bit per channel.
    uint32_t finished() const { return fin_; }
    uint32_t half_finished() const { return half_; }
    void     acknowledge(uint32_t finished, uint32_t half_finished);
    // A running channel is stuck at fixed priority 0.
    bool stuck() const;

    bool priority_mode() const { return prio_; }
    void set_priority_mode(bool fixed) { prio_ = fixed; }

    void                         tick(uint64_t now_ns);
    std::optional<uint64_t>      next_event_ns() const;
    void                         reset();
    std::shared_ptr<RequestLine> request_line(unsigned n);

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
        uint32_t               config = 0;
        Words                  words;
        bool                   running = false, stalled = false;
        bool                   stuck = false; // fixed priority 0: never granted until enable is cleared
        std::unique_ptr<Mover> mover;         // kept after a stop or a finish: remain and dst_wr_addr
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

    uint32_t    src_request(uint32_t mux) const;
    uint32_t    dst_request(uint32_t mux) const;
    bool        is_memory(uint32_t request) const;
    unsigned    priority(uint32_t config) const;
    std::string endpoint(uint32_t request, uint32_t address) const;
    std::string describe(const Channel &channel) const;

    void                             start(unsigned index, Channel &channel);
    void                             begin_cycle(Channel &channel, uint64_t src, uint64_t dst);
    void                             advance(uint64_t now_ns);
    std::array<double, kMaxChannels> rates(const std::array<bool, kMaxChannels> &held) const;
    // Moves the words `progress` allows; false when the channel was aborted.
    bool move(unsigned index, Channel &channel);
    // Handles the event at the channel's target; true when its flag is enabled.
    bool   reach_target(unsigned index, Channel &channel);
    double target(const Channel &channel) const;
    bool   has_event(const Channel &channel) const;
    void   run_paced(unsigned index, Channel &channel, uint64_t now_ns);
    void   half_finished(unsigned index, Channel &channel);
    void   end_cycle(unsigned index, Channel &channel);
    void   flag_finished(unsigned index, Channel &channel);
    void   abort(unsigned index, Channel &channel, const BusError &err);
    void   update_irq();

    const Clock                                     &clock_;
    Line                                             irq_;
    BusMemory                                       &memory_;
    uint32_t                                         bus_hz_;
    unsigned                                         count_;
    Encoding                                         encoding_;
    std::array<Channel, kMaxChannels>                channels_;
    std::map<unsigned, std::shared_ptr<RequestLine>> ports_;
    uint32_t                                         fin_ = 0, half_ = 0;
    bool                                             prio_        = false;
    uint64_t                                         advanced_ns_ = 0; // device time the channels were moved to
};

} // namespace bk
