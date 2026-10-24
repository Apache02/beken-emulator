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
// incrementing. An under-filled last write is zero padded.
//
// Memory-to-memory channels copy when the modelled transfer time has passed,
// so a firmware polling `enable` sees it set meanwhile. Channels whose source
// is a peripheral with a request line move a word whenever the peripheral
// holds its line up. Any other request line, and repeat mode, are reported and
// not emulated (the channel stalls, repeat runs once).
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
    // One channel's data movement; it can stop in the middle when a request
    // line source has no word and carry on later.
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
              std::shared_ptr<RequestLine> port = {});

        bool done() const { return writes >= writes_total; }
        // Does up to `max_writes` destination writes (all when empty) and
        // returns how many were done. Throws BusError.
        uint64_t run(std::optional<uint64_t> max_writes);

        uint64_t src, dst;
        uint64_t writes_total, writes = 0, reads = 0;

      private:
        BusMemory                   &memory_;
        std::shared_ptr<RequestLine> port_;
        uint32_t                     length_;
        uint64_t                     src_bytes_, dst_bytes_;
        bool                         src_inc_, dst_inc_;
        uint32_t                     taken_ = 0;
        std::vector<uint8_t>         accumulator_;
    };

    struct Channel {
        uint32_t                     config = 0, dst = 0, src = 0, mux = 0;
        std::array<uint32_t, 4>      loops{};
        std::array<uint32_t, 2>      pause{};
        bool                         running = false, stalled = false;
        uint64_t                     start_ns = 0, end_ns = 0, half_ns = 0;
        uint64_t                     writes       = 0;
        bool                         half_pending = false;
        uint32_t                     fin_count = 0, half_count = 0;
        std::unique_ptr<Mover>       mover;         // kept after a stop: it holds the write position
        bool                         paced = false; // a request line decides when words move
        std::shared_ptr<RequestLine> port;
        uint64_t                     word_ns = 0, last_ns = 0;
    };

    uint32_t read_channel(const Channel &channel, uint32_t within) const;
    void     write_channel(unsigned index, uint32_t within, uint32_t value);
    void     write_config(unsigned index, Channel &channel, uint32_t value);
    void     start(unsigned index, Channel &channel);
    void     run_paced(unsigned index, Channel &channel, uint64_t now_ns);
    void     half_finished(unsigned index, Channel &channel);
    void     finish(unsigned index, Channel &channel);
    void     flag_finished(unsigned index, Channel &channel);
    void     acknowledge(uint32_t value);
    uint32_t int_counts(unsigned index) const;
    uint32_t dst_wr_addr(unsigned index) const;
    uint64_t done_writes(const Channel &channel) const;
    void     update_irq();

    std::string                                      name_;
    const Clock                                     &clock_;
    Line                                             irq_;
    BusMemory                                       &memory_;
    uint32_t                                         bus_hz_;
    std::array<Channel, kChannels>                   channels_;
    std::map<unsigned, std::shared_ptr<RequestLine>> ports_;
    uint32_t                                         fin_ = 0, half_ = 0, prio_ = 0;
};

} // namespace bk
