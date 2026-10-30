#include "periph/jpeg.h"

#include <utility>

namespace bk {

namespace {

constexpr uint32_t kCtrl0 = 0x00, kCtrl1 = 0x04, kTargetH = 0x08, kTargetL = 0x0C, kFifo = 0x14, kStatus = 0x18,
                   kBytes = 0x1C, kState = 0x20, kQuant = 0x80;
constexpr uint32_t kStartInt = 1u << 2, kEndInt = 1u << 3;
constexpr uint32_t kByteRev = 1u << 1, kEncEn = 1u << 4, kEncSize = 1u << 16;

} // namespace

Jpeg::Jpeg(std::string                  name,
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

void Jpeg::reset() {
    ctrl0_ = ctrl1_ = 0;
    targets_        = {};
    stream_.reset();
}

uint32_t Jpeg::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kCtrl0: return ctrl0_;
        case kCtrl1: return ctrl1_;
        case kTargetH: return targets_[0];
        case kTargetL: return targets_[1];
        case kFifo: return stream_.pop();
        case kStatus: return stream_.events();
        case kBytes: return stream_.last_size();
        case kState: return stream_.empty() ? 1 : 0;
        default:
            if (offset >= kQuant && offset < kQuant + 4 * quant_.size() && offset % 4 == 0)
                return quant_[(offset - kQuant) / 4];
            return 0;
    }
}

void Jpeg::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kCtrl0: ctrl0_ = value; break;
        case kCtrl1:
            ctrl1_ = value;
            stream_.set_format((value & kByteRev) != 0, (value & kEncSize) != 0);
            stream_.enable((value & kEncEn) != 0);
            break;
        case kTargetH: targets_[0] = value; break;
        case kTargetL: targets_[1] = value; break;
        case kStatus: stream_.clear_events(value); break;
        default:
            if (offset >= kQuant && offset < kQuant + 4 * quant_.size() && offset % 4 == 0)
                quant_[(offset - kQuant) / 4] = value;
            break;
    }
    drive();
}

void Jpeg::drive() {
    const uint32_t events = stream_.events();
    if (irq_)
        irq_(((events & JpegStream::kStart) && (ctrl0_ & kStartInt)) ||
             ((events & JpegStream::kEnd) && (ctrl0_ & kEndInt)));
}

} // namespace bk
