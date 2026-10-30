#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"
#include "periph/jpeg_stream.h"

namespace bk {

// The BK7221U JPEG encoder and DVP interface, without a sensor: frames come
// from a JpegStream. Registers: ctrl0 at 0x00 (start_frame and end_frame
// interrupt enables in bits 2 and 3), ctrl1 at 0x04 (video_byte_rev bit 1,
// jpeg_enc_en bit 4, enc_size bit 16), target bytes at 0x08 and 0x0C,
// rx_fifo_data at 0x14, the start_frame and end_frame flags at 0x18 (bits 0
// and 1, write 1 to clear), the byte count of the last frame at 0x1C, 1 at
// 0x20 while the FIFO is empty, the quantization table at 0x80.
//
// The picture size, quantization table contents, sync polarity, input format
// and bit-rate control have no effect.
class Jpeg : public Module {
  public:
    // See JpegStream for `picture`, the timing and `burst_words`. `irq` and
    // `dma` may be empty.
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
    void                    tick(uint64_t now_ns) override { stream_.tick(now_ns); }
    std::optional<uint64_t> next_event_ns() const override { return stream_.next_event_ns(); }
    // Clears everything but the quantization table.
    void reset() override;

  private:
    void drive();

    std::string              name_;
    Line                     irq_;
    uint32_t                 ctrl0_ = 0, ctrl1_ = 0;
    std::array<uint32_t, 2>  targets_{};
    std::array<uint32_t, 32> quant_{}; // survives a reset
    JpegStream               stream_;
};

} // namespace bk
