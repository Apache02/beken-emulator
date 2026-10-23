#include "periph/jpeg.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr uint32_t kCtrl0 = 0x00, kCtrl1 = 0x04, kTargetH = 0x08, kTargetL = 0x0C, kFifo = 0x14, kStatus = 0x18,
                   kBytes = 0x1C, kState = 0x20, kQuant = 0x80;
constexpr uint32_t kStartInt = 1u << 2, kEndInt = 1u << 3;
constexpr uint32_t kByteRev = 1u << 1, kEncEn = 1u << 4, kEncSize = 1u << 16;
constexpr uint32_t kStart = 1, kEnd = 2;
constexpr size_t   kTrailerBytes = 5;

Logger &log() { return Logger::get("jpeg"); }

// `jpeg`, with a comment segment after the SOI marker when needed so that it
// plus the trailer fills whole 32-bit words.
std::vector<uint8_t> aligned(std::vector<uint8_t> jpeg) {
    if ((jpeg.size() + kTrailerBytes) % 4 == 0) return jpeg;
    const size_t         pad     = (4 - (jpeg.size() + 4 + kTrailerBytes) % 4) % 4;
    std::vector<uint8_t> comment = {0xFF, 0xFE, static_cast<uint8_t>((2 + pad) >> 8), static_cast<uint8_t>(2 + pad)};
    comment.insert(comment.end(), pad, 0);
    jpeg.insert(jpeg.begin() + 2, comment.begin(), comment.end());
    return jpeg;
}

} // namespace

Jpeg::Jpeg(std::string                  name,
           const Clock                 &clock,
           Line                         irq,
           std::shared_ptr<RequestLine> dma,
           uint32_t                     frame_interval_ms,
           uint32_t                     encode_ms,
           std::vector<uint8_t>         picture)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)), dma_(std::move(dma)),
      interval_ns_(uint64_t{frame_interval_ms} * 1'000'000), encode_ns_(uint64_t{encode_ms} * 1'000'000) {
    if (picture.size() < 2) throw std::invalid_argument(name_ + ": the picture is too short to be a JPEG");
    picture_ = aligned(std::move(picture));
    if (dma_) {
        dma_->bind([this] {
            const uint32_t word = pop();
            return std::array<uint8_t, 4>{static_cast<uint8_t>(word),
                                          static_cast<uint8_t>(word >> 8),
                                          static_cast<uint8_t>(word >> 16),
                                          static_cast<uint8_t>(word >> 24)};
        });
    }
    reset();
}

void Jpeg::reset() {
    ctrl0_ = ctrl1_ = status_ = 0;
    targets_                  = {};
    last_size_ = frame_size_ = last_word_ = 0;
    stream_.clear();
    offset_ = 0;
    start_ns_.reset();
    end_ns_.reset();
    drive();
}

uint32_t Jpeg::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kCtrl0: return ctrl0_;
        case kCtrl1: return ctrl1_;
        case kTargetH: return targets_[0];
        case kTargetL: return targets_[1];
        case kFifo: return pop();
        case kStatus: return status_;
        case kBytes: return last_size_;
        case kState: return exhausted() ? 1 : 0;
        default:
            if (offset >= kQuant && offset < kQuant + 4 * quant_.size() && offset % 4 == 0)
                return quant_[(offset - kQuant) / 4];
            return 0;
    }
}

void Jpeg::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kCtrl0: ctrl0_ = value; break;
        case kCtrl1: write_ctrl1(value); break;
        case kTargetH: targets_[0] = value; break;
        case kTargetL: targets_[1] = value; break;
        case kStatus: status_ &= ~value; break;
        default:
            if (offset >= kQuant && offset < kQuant + 4 * quant_.size() && offset % 4 == 0)
                quant_[(offset - kQuant) / 4] = value;
            break;
    }
    drive();
}

std::optional<uint64_t> Jpeg::next_event_ns() const {
    if (start_ns_ && end_ns_) return std::min(*start_ns_, *end_ns_);
    return start_ns_ ? start_ns_ : end_ns_;
}

void Jpeg::tick(uint64_t now_ns) {
    while (true) {
        std::optional<uint64_t> due = next_event_ns();
        if (!due || *due > now_ns) break;
        if (end_ns_ && *due == *end_ns_)
            end_frame();
        else
            start_frame(*due);
    }
    drive();
}

void Jpeg::write_ctrl1(uint32_t value) {
    const bool was_enabled = (ctrl1_ & kEncEn) != 0;
    ctrl1_                 = value;
    const bool enabled     = (value & kEncEn) != 0;
    if (enabled && !was_enabled) {
        start_ns_ = clock_.now_ns() + interval_ns_;
        log().log(Level::Debug, "encoder enabled, first frame in " + std::to_string(interval_ns_ / 1'000'000) + " ms");
    } else if (was_enabled && !enabled) {
        start_ns_.reset();
        end_ns_.reset();
        stream_.clear();
        offset_ = 0;
        log().log(Level::Debug, "encoder disabled");
    }
}

void Jpeg::start_frame(uint64_t at_ns) {
    frame_size_ = static_cast<uint32_t>(picture_.size());
    stream_     = picture_;
    if (ctrl1_ & kEncSize) {
        stream_.push_back(0);
        for (int i = 0; i < 4; ++i) stream_.push_back(static_cast<uint8_t>(frame_size_ >> (8 * i)));
    }
    stream_.resize((stream_.size() + 3) & ~size_t{3}, 0);
    offset_ = 0;
    status_ |= kStart;
    start_ns_ = at_ns + interval_ns_;
    end_ns_   = at_ns + encode_ns_;
    log().log(Level::Debug, "start_frame: " + std::to_string(stream_.size()) + " bytes");
}

void Jpeg::end_frame() {
    last_size_ = frame_size_;
    status_ |= kEnd;
    end_ns_.reset();
    log().log(Level::Debug, "end_frame: frame is " + std::to_string(last_size_) + " bytes");
}

// The next word of rx_fifo_data: the next four stream bytes, little endian
// with video_byte_rev set, else big endian. Once the stream is exhausted, the
// last word that was read.
uint32_t Jpeg::pop() {
    if (exhausted()) return last_word_;
    uint32_t word = 0;
    for (size_t i = 0; i < 4; ++i) {
        const uint32_t byte = stream_[offset_ + i];
        word |= (ctrl1_ & kByteRev) ? byte << (8 * i) : byte << (8 * (3 - i));
    }
    offset_ += 4;
    last_word_ = word;
    drive();
    return word;
}

void Jpeg::drive() {
    if (irq_) irq_(((status_ & kStart) && (ctrl0_ & kStartInt)) || ((status_ & kEnd) && (ctrl0_ & kEndInt)));
    if (dma_) dma_->set(!exhausted());
}

} // namespace bk
