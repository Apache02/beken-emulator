#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"

namespace bk {

// JPEG encoder and DVP interface without a sensor. While the encoder is
// enabled a picture is "captured" once per frame interval: start_frame starts
// a finished JPEG on the output FIFO (read position back to the first word) and
// end_frame follows a fixed time later. Words come out of `rx_fifo_data` until
// the FIFO is empty, and a DMA channel with the JPEG request line drains it
// while the line is up. Once it is empty `rx_fifo_data` keeps returning the
// last word that was read; with `enc_size` set that is the frame-size trailer.
//
// The encoder produces the stream at the pace of the sensor: `burst_words`
// words reach the FIFO at a time, evenly spread over the encode time, so that
// the last ones arrive when the frame ends. With `burst_words` 0 the whole
// frame is in the FIFO from the start of the frame.
//
// The picture size, quantization table contents, sync polarity, input format
// and bit-rate control have no effect.
class Jpeg : public Module {
  public:
    // `picture` is a JPEG file; it is made to fit whole 32-bit words together
    // with the 5-byte trailer by inserting a comment segment after the SOI
    // marker. Throws std::invalid_argument when it is shorter than 2 bytes.
    // `irq` and `dma` may be empty.
    Jpeg(std::string                  name,
         const Clock                 &clock,
         Line                         irq,
         std::shared_ptr<RequestLine> dma,
         uint32_t                     frame_interval_ms,
         uint32_t                     encode_ms,
         std::vector<uint8_t>         picture,
         uint32_t                     burst_words = 0);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x100}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override;
    std::optional<uint64_t> next_event_ns() const override;
    // Clears everything but the quantization table.
    void reset() override;

  private:
    void     write_ctrl1(uint32_t value);
    void     start_frame(uint64_t at_ns);
    void     end_frame();
    void     release();
    uint32_t pop();
    bool     empty() const { return offset_ >= released_; }
    void     drive();

    std::string                  name_;
    const Clock                 &clock_;
    Line                         irq_;
    std::shared_ptr<RequestLine> dma_;
    uint64_t                     interval_ns_, encode_ns_;
    std::vector<uint8_t>         picture_;
    uint32_t                     burst_words_;

    uint32_t                 ctrl0_ = 0, ctrl1_ = 0, status_ = 0;
    std::array<uint32_t, 2>  targets_{};
    std::array<uint32_t, 32> quant_{}; // survives a reset
    uint32_t                 last_size_ = 0, frame_size_ = 0, last_word_ = 0;
    std::vector<uint8_t>     stream_;
    size_t                   offset_ = 0, released_ = 0; // bytes of stream_ that have reached the FIFO
    uint64_t                 frame_ns_ = 0;              // when the current frame started
    std::optional<uint64_t>  start_ns_, end_ns_, release_ns_;
};

} // namespace bk
