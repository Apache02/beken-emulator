#include "periph/jpeg_stream.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/log.h"

namespace bk {

namespace {

constexpr size_t kTrailerBytes = 5;

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

JpegStream::JpegStream(const std::string           &name,
                       const Clock                 &clock,
                       std::shared_ptr<RequestLine> dma,
                       uint32_t                     frame_interval_ms,
                       uint32_t                     encode_ms,
                       std::vector<uint8_t>         picture,
                       uint32_t                     burst_words,
                       std::function<void()>        changed)
    : clock_(clock), dma_(std::move(dma)), interval_ns_(uint64_t{frame_interval_ms} * 1'000'000),
      encode_ns_(uint64_t{encode_ms} * 1'000'000), burst_words_(burst_words), changed_(std::move(changed)) {
    if (picture.size() < 2) throw std::invalid_argument(name + ": the picture is too short to be a JPEG");
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
}

void JpegStream::reset() {
    enabled_ = byte_reverse_ = size_trailer_ = false;
    events_                                  = 0;
    last_size_ = frame_size_ = last_word_ = 0;
    stream_.clear();
    offset_ = released_ = 0;
    start_ns_.reset();
    end_ns_.reset();
    release_ns_.reset();
    drive();
}

void JpegStream::enable(bool on) {
    if (on && !enabled_) {
        start_ns_ = clock_.now_ns() + interval_ns_;
        log().log(Level::Debug, "encoder enabled, first frame in " + std::to_string(interval_ns_ / 1'000'000) + " ms");
    } else if (!on && enabled_) {
        start_ns_.reset();
        end_ns_.reset();
        release_ns_.reset();
        stream_.clear();
        offset_ = released_ = 0;
        log().log(Level::Debug, "encoder disabled");
    }
    enabled_ = on;
    drive();
}

void JpegStream::set_format(bool byte_reverse, bool size_trailer) {
    byte_reverse_ = byte_reverse;
    size_trailer_ = size_trailer;
}

void JpegStream::clear_events(uint32_t mask) {
    events_ &= ~mask;
    drive();
}

std::optional<uint64_t> JpegStream::next_event_ns() const {
    std::optional<uint64_t> next;
    for (const auto &t : {start_ns_, end_ns_, release_ns_}) {
        if (t && (!next || *t < *next)) next = t;
    }
    return next;
}

// At the same time words reach the FIFO before the frame ends, and a frame
// ends before the next one starts.
void JpegStream::tick(uint64_t now_ns) {
    while (true) {
        std::optional<uint64_t> due = next_event_ns();
        if (!due || *due > now_ns) break;
        if (release_ns_ && *due == *release_ns_)
            release();
        else if (end_ns_ && *due == *end_ns_)
            end_frame();
        else
            start_frame(*due);
    }
    drive();
}

void JpegStream::start_frame(uint64_t at_ns) {
    frame_size_ = static_cast<uint32_t>(picture_.size());
    stream_     = picture_;
    if (size_trailer_) {
        stream_.push_back(0);
        for (int i = 0; i < 4; ++i) stream_.push_back(static_cast<uint8_t>(frame_size_ >> (8 * i)));
    }
    stream_.resize((stream_.size() + 3) & ~size_t{3}, 0);
    offset_   = 0;
    frame_ns_ = at_ns;
    events_ |= kStart;
    start_ns_ = at_ns + interval_ns_;
    end_ns_   = at_ns + encode_ns_;
    release_ns_.reset();
    if (burst_words_ == 0) {
        released_ = stream_.size();
    } else {
        released_   = 0;
        release_ns_ = at_ns + encode_ns_ * std::min<uint64_t>(burst_words_, stream_.size() / 4) / (stream_.size() / 4);
    }
    log().log(Level::Debug, "start_frame: " + std::to_string(stream_.size()) + " bytes");
}

// The next burst of words reaches the FIFO.
void JpegStream::release() {
    const size_t words = stream_.size() / 4;
    released_          = std::min<size_t>(stream_.size(), released_ + 4 * size_t{burst_words_});
    if (released_ >= stream_.size()) {
        release_ns_.reset();
        return;
    }
    const uint64_t next = std::min<uint64_t>(released_ / 4 + burst_words_, words);
    release_ns_         = frame_ns_ + encode_ns_ * next / words;
}

void JpegStream::end_frame() {
    last_size_ = frame_size_;
    events_ |= kEnd;
    end_ns_.reset();
    log().log(Level::Debug, "end_frame: frame is " + std::to_string(last_size_) + " bytes");
}

uint32_t JpegStream::pop() {
    if (empty()) return last_word_;
    uint32_t word = 0;
    for (size_t i = 0; i < 4; ++i) {
        const uint32_t byte = stream_[offset_ + i];
        word |= byte_reverse_ ? byte << (8 * i) : byte << (8 * (3 - i));
    }
    offset_ += 4;
    last_word_ = word;
    drive();
    return word;
}

void JpegStream::drive() {
    if (dma_) dma_->set(!empty());
    if (changed_) changed_();
}

} // namespace bk
