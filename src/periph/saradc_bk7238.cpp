#include "periph/saradc_bk7238.h"

#include <algorithm>
#include <utility>

namespace bk {

namespace {

constexpr uint32_t kConfig0 = 0x00, kConfig1 = 0x04, kConfig2 = 0x08, kState = 0x0C, kData = 0x10, kConfig5 = 0x14,
                   kConfig6   = 0x18;
constexpr uint32_t kStart     = 1u << 0;
constexpr uint32_t kLevelMask = 0x7, kFifoIntEn = 1u << 3, kFifoInt = 1u << 4, kConvIntEn = 1u << 14,
                   kConvDone = 1u << 15, kTimeoutIntEn = 1u << 16, kTimeoutInt = 1u << 17, kEmpty = 1u << 30,
                   kFull     = 1u << 31;
constexpr uint32_t kEnables  = kFifoIntEn | kConvIntEn | kTimeoutIntEn;
constexpr uint32_t kFlags    = kFifoInt | kConvDone | kTimeoutInt;
constexpr uint32_t kDepth    = 8;
constexpr uint64_t kSampleNs = 100'000;

} // namespace

SaradcBk7238::SaradcBk7238(std::string name, Line irq, uint32_t sample)
    : name_(std::move(name)), irq_(std::move(irq)), sample_(sample) {}

uint32_t SaradcBk7238::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kConfig0: return config0_;
        case kConfig1: return config1_;
        case kConfig2: return config2_;
        case kState: return state();
        case kData:
            if (fifo_ == 0) return 0;
            --fifo_;
            return sample_ & 0xFFFF;
        case kConfig5: return config5_;
        case kConfig6: return config6_;
        default: return 0;
    }
}

void SaradcBk7238::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kConfig0: config0_ = value; break;
        case kConfig1: config1_ = value; break;
        case kConfig2: config2_ = value; break;
        case kState: state_ = (value & ~(kLevelMask | kFlags | kEmpty | kFull)) | (state_ & kFlags & ~value); break;
        case kConfig5: config5_ = value; break;
        case kConfig6: config6_ = value; break;
        default: break;
    }
    update_schedule();
    update_irq();
}

void SaradcBk7238::tick(uint64_t now_ns) {
    last_tick_ = now_ns;
    while (next_ns_ && now_ns >= *next_ns_) {
        if (fifo_ < kDepth) ++fifo_;
        state_ |= kFifoInt | kConvDone;
        *next_ns_ += kSampleNs;
    }
    update_irq();
}

bool SaradcBk7238::running() const { return (config0_ & kStart) && (state_ & kEnables); }

// Starting schedules the first sample 100 us after the last tick; stopping
// drops it but keeps the FIFO, which the driver drains.
void SaradcBk7238::update_schedule() {
    if (running() && !next_ns_) {
        next_ns_ = last_tick_ + kSampleNs;
    } else if (!running()) {
        next_ns_.reset();
    }
}

uint32_t SaradcBk7238::state() const {
    return state_ | std::min(fifo_, kLevelMask) | (fifo_ == 0 ? kEmpty : 0) | (fifo_ >= kDepth ? kFull : 0);
}

void SaradcBk7238::update_irq() {
    if (!irq_) return;
    const bool fifo    = (state_ & kFifoInt) && (state_ & kFifoIntEn);
    const bool conv    = (state_ & kConvDone) && (state_ & kConvIntEn);
    const bool timeout = (state_ & kTimeoutInt) && (state_ & kTimeoutIntEn);
    irq_(fifo || conv || timeout);
}

void SaradcBk7238::reset() {
    config0_ = config1_ = config2_ = config5_ = config6_ = 0;
    state_                                               = 0;
    last_tick_                                           = 0;
    fifo_                                                = 0;
    next_ns_.reset();
    update_irq();
}

} // namespace bk
