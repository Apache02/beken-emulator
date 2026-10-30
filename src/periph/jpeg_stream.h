#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/module.h"

namespace bk {

// The frames of a JPEG encoder without a sensor, shared by the JPEG modules,
// which differ only in their register layout. It holds no registers.
//
// While the encoder is enabled a picture is "captured" once per frame
// interval: the start of a frame puts a finished JPEG on the output FIFO (read
// position back to the first word) and its end follows a fixed time later.
// Words come out of pop() until the FIFO is empty, and a DMA channel with the
// JPEG request line drains it while the line is up. Once it is empty pop()
// keeps returning the last word that was read; with the frame size trailer on
// that is the trailer.
//
// The encoder produces the stream at the pace of the sensor: `burst_words`
// words reach the FIFO at a time, evenly spread over the encode time, so that
// the last ones arrive when the frame ends. With `burst_words` 0 the whole
// frame is in the FIFO from the start of the frame.
class JpegStream {
  public:
    // The events of a frame; they stay until cleared.
    static constexpr uint32_t kStart = 1, kEnd = 2;

    // `picture` is a JPEG file; it is made to fit whole 32-bit words together
    // with the 5-byte trailer by inserting a comment segment after the SOI
    // marker. Throws std::invalid_argument (naming `name`) when it is shorter
    // than 2 bytes. `dma` may be empty. `changed` is called whenever the events
    // may have changed, so that the module can drive its interrupt line.
    JpegStream(const std::string           &name,
               const Clock                 &clock,
               std::shared_ptr<RequestLine> dma,
               uint32_t                     frame_interval_ms,
               uint32_t                     encode_ms,
               std::vector<uint8_t>         picture,
               uint32_t                     burst_words,
               std::function<void()>        changed);

    // Enabling schedules the first frame one interval later; disabling drops
    // the current frame.
    void enable(bool on);
    bool enabled() const { return enabled_; }
    // byte_reverse: words are little endian, else big endian. size_trailer: a
    // zero byte and the frame size (4 bytes, little endian) follow the picture.
    void set_format(bool byte_reverse, bool size_trailer);

    // The next word of the FIFO, or the last word read while it is empty.
    uint32_t pop();
    bool     empty() const { return offset_ >= released_; }
    // The size of the last frame that ended (without the trailer), 0 before.
    uint32_t last_size() const { return last_size_; }

    uint32_t events() const { return events_; }
    void     clear_events(uint32_t mask);

    void                    tick(uint64_t now_ns);
    std::optional<uint64_t> next_event_ns() const;
    void                    reset();

  private:
    void start_frame(uint64_t at_ns);
    void end_frame();
    void release();
    void drive();

    const Clock                 &clock_;
    std::shared_ptr<RequestLine> dma_;
    uint64_t                     interval_ns_, encode_ns_;
    std::vector<uint8_t>         picture_;
    uint32_t                     burst_words_;
    std::function<void()>        changed_;

    bool                    enabled_ = false, byte_reverse_ = false, size_trailer_ = false;
    uint32_t                events_    = 0;
    uint32_t                last_size_ = 0, frame_size_ = 0, last_word_ = 0;
    std::vector<uint8_t>    stream_;
    size_t                  offset_ = 0, released_ = 0; // bytes of stream_ that have reached the FIFO
    uint64_t                frame_ns_ = 0;              // when the current frame started
    std::optional<uint64_t> start_ns_, end_ns_, release_ns_;
};

} // namespace bk
