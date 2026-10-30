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

// The BK7252N JPEG encoder, without a sensor: frames come from a JpegStream.
// Registers (vendor SDK names): dev_id and ver_id (read 0), clk_ctrl at 0x08,
// global_status at 0x0C (reads 0), eof_offset at 0x10, rx_fifo_data at 0x14,
// int_status at 0x18 (sof bit 0, eof bit 1; head, err and line clear are
// never raised; write 1 to clear), the byte count of the last frame minus 1
// at 0x1C (the driver adds 1), the partial-window words at 0x20 and 0x24
// (0x20 reads stream_fifo_empty in bit 26), byte_cnt_line at 0x28, the
// interrupt enables at 0x30 (sof bit 2, eof bit 3), the encoder config at
// 0x34 (video_byte_rev bit 1, jpeg_enc_en bit 4, jpeg_enc_size bit 16), the
// target bytes at 0x38 and 0x3C and the quantization table at 0x80.
//
// The picture size, the YUV buffer, the quantization table contents, sync
// polarity, input format and bit-rate control have no effect.
class JpegBk7252n : public Module {
  public:
    // See JpegStream for `picture`, the timing and `burst_words`. `irq` and
    // `dma` may be empty.
    JpegBk7252n(std::string                  name,
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
    uint32_t                 clk_ctrl_ = 0, eof_offset_ = 0, partial_x_ = 0, partial_y_ = 0;
    uint32_t                 int_enable_ = 0, config_ = 0;
    std::array<uint32_t, 2>  targets_{};
    std::array<uint32_t, 32> quant_{}; // survives a reset
    JpegStream               stream_;
};

} // namespace bk
