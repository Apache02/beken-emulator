#include "periph/jpeg_bk7252n.h"

#include <utility>

namespace bk {

namespace {

constexpr uint32_t kClkCtrl = 0x08, kEofOffset = 0x10, kFifo = 0x14, kIntStatus = 0x18, kBytes = 0x1C, kPartialX = 0x20,
                   kPartialY = 0x24, kIntEnable = 0x30, kConfig = 0x34, kTargetH = 0x38, kTargetL = 0x3C, kQuant = 0x80;
constexpr uint32_t kSofInt = 1u << 2, kEofInt = 1u << 3;
constexpr uint32_t kByteRev = 1u << 1, kEncEn = 1u << 4, kEncSize = 1u << 16;
constexpr uint32_t kFifoEmpty = 1u << 26, kFifoFull = 1u << 27;

} // namespace

JpegBk7252n::JpegBk7252n(std::string                  name,
                         const Clock                 &clock,
                         Line                         irq,
                         std::shared_ptr<RequestLine> dma,
                         uint32_t                     frame_interval_ms,
                         uint32_t                     encode_ms,
                         std::vector<uint8_t>         picture,
                         uint32_t                     burst_words)
    : name_(std::move(name)), irq_(std::move(irq)),
      stream_(name_, clock, std::move(dma), frame_interval_ms, encode_ms, std::move(picture), burst_words, [this] {
          drive();
      }) {
    reset();
}

void JpegBk7252n::reset() {
    clk_ctrl_ = eof_offset_ = partial_x_ = partial_y_ = 0;
    int_enable_ = config_ = 0;
    targets_              = {};
    stream_.reset();
}

uint32_t JpegBk7252n::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kClkCtrl: return clk_ctrl_;
        case kEofOffset: return eof_offset_;
        case kFifo: return stream_.pop();
        case kIntStatus: return stream_.events();
        case kBytes: return stream_.last_size() ? stream_.last_size() - 1 : 0;
        case kPartialX: return (partial_x_ & ~(kFifoEmpty | kFifoFull)) | (stream_.empty() ? kFifoEmpty : 0);
        case kPartialY: return partial_y_;
        case kIntEnable: return int_enable_;
        case kConfig: return config_;
        case kTargetH: return targets_[0];
        case kTargetL: return targets_[1];
        default:
            if (offset >= kQuant && offset < kQuant + 4 * quant_.size() && offset % 4 == 0)
                return quant_[(offset - kQuant) / 4];
            return 0;
    }
}

void JpegBk7252n::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kClkCtrl: clk_ctrl_ = value; break;
        case kEofOffset: eof_offset_ = value; break;
        case kIntStatus: stream_.clear_events(value); break;
        case kPartialX: partial_x_ = value; break;
        case kPartialY: partial_y_ = value; break;
        case kIntEnable: int_enable_ = value; break;
        case kConfig:
            config_ = value;
            stream_.set_format((value & kByteRev) != 0, (value & kEncSize) != 0);
            stream_.enable((value & kEncEn) != 0);
            break;
        case kTargetH: targets_[0] = value; break;
        case kTargetL: targets_[1] = value; break;
        default:
            if (offset >= kQuant && offset < kQuant + 4 * quant_.size() && offset % 4 == 0)
                quant_[(offset - kQuant) / 4] = value;
            break;
    }
    drive();
}

void JpegBk7252n::drive() {
    const uint32_t events = stream_.events();
    if (irq_)
        irq_(((events & JpegStream::kStart) && (int_enable_ & kSofInt)) ||
             ((events & JpegStream::kEnd) && (int_enable_ & kEofInt)));
}

} // namespace bk
