#include "periph/uart.h"

#include <algorithm>
#include <utility>

namespace bk {

namespace {
constexpr uint32_t kConfig = 0x00, kFifoConfig = 0x04, kFifoStatus = 0x08, kData = 0x0C, kIntEnable = 0x10,
                   kIntStatus = 0x14;

constexpr uint32_t kTxNeedWrite = 1u << 0, kRxNeedRead = 1u << 1, kTxStopEnd = 1u << 5, kRxStopEnd = 1u << 6,
                   kLatchedMask = kTxStopEnd | kRxStopEnd;

constexpr uint64_t kStopEndDelayNs = 1'000'000;
constexpr uint64_t kTxBatchNs      = 1'000'000;
} // namespace

Uart::Uart(std::string name, const Clock &clock, Line irq)
    : name_(std::move(name)), clock_(clock), irq_(std::move(irq)) {}

uint32_t Uart::raw() const {
    uint32_t bits      = kTxNeedWrite | latched_;
    uint32_t threshold = (fifo_config_ >> 8) & 0xFF;
    if (!fifo_.empty() && fifo_.size() >= threshold) bits |= kRxNeedRead;
    return bits;
}

uint32_t Uart::fifo_status() const {
    uint32_t count = static_cast<uint32_t>(fifo_.size());
    uint32_t v     = count << 8 | 1u << 17 | 1u << 20;
    if (count == kDepth) v |= 1u << 18;
    if (count == 0) v |= 1u << 19;
    if (count > 0) v |= 1u << 21;
    return v;
}

void Uart::refill() {
    bool moved = false;
    while (!backlog_.empty() && fifo_.size() < kDepth) {
        fifo_.push_back(backlog_.front());
        backlog_.pop_front();
        moved = true;
    }
    if (moved) stop_deadline_ = last_tick_ + kStopEndDelayNs;
}

void Uart::update_line() {
    if (irq_) irq_((raw() & int_enable_) != 0);
}

uint32_t Uart::read(const std::string &, uint32_t offset, uint32_t) {
    uint32_t v = 0;
    switch (offset) {
        case kConfig: v = words_[0]; break;
        case kFifoConfig: v = fifo_config_; break;
        case kFifoStatus: v = fifo_status(); break;
        case kData:
            if (!fifo_.empty()) {
                v = static_cast<uint32_t>(fifo_.front()) << 8;
                fifo_.pop_front();
                refill();
                update_line();
            }
            break;
        case kIntEnable: v = int_enable_; break;
        case kIntStatus: v = raw() & int_enable_; break;
        default: v = words_[(offset >> 2) % words_.size()]; break;
    }
    return v;
}

void Uart::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kConfig: words_[0] = value; break;
        case kFifoConfig: fifo_config_ = value; break;
        case kFifoStatus: break;
        case kData: {
            if (endpoint_) {
                if (tx_.empty()) tx_deadline_ = clock_.now_ns() + kTxBatchNs;
                tx_.push_back(static_cast<uint8_t>(value & 0xFF));
            }
            latched_ |= kTxStopEnd;
            break;
        }
        case kIntEnable: int_enable_ = value & 0xFF; break;
        case kIntStatus: latched_ &= ~(value & kLatchedMask); break;
        default: words_[(offset >> 2) % words_.size()] = value; break;
    }
    update_line();
}

void Uart::flush_tx() {
    if (endpoint_ && !tx_.empty()) endpoint_->write(tx_);
    tx_.clear();
    tx_deadline_.reset();
}

std::optional<uint64_t> Uart::next_event_ns() const {
    if (stop_deadline_ && tx_deadline_) return std::min(*stop_deadline_, *tx_deadline_);
    return stop_deadline_ ? stop_deadline_ : tx_deadline_;
}

void Uart::tick(uint64_t now_ns) {
    last_tick_ = now_ns;
    if (tx_deadline_ && now_ns >= *tx_deadline_) flush_tx();
    if (endpoint_) {
        std::vector<uint8_t> in = endpoint_->read_nowait();
        backlog_.insert(backlog_.end(), in.begin(), in.end());
    }
    refill();
    if (stop_deadline_ && now_ns >= *stop_deadline_) {
        latched_ |= kRxStopEnd;
        stop_deadline_.reset();
    }
    update_line();
}

void Uart::reset() {
    fifo_.clear();
    backlog_.clear();
    flush_tx(); // bytes the firmware already sent still reach the host
    fifo_config_ = int_enable_ = latched_ = 0;
    words_                                = {};
    last_tick_                            = 0;
    stop_deadline_.reset();
    update_line();
}

} // namespace bk
