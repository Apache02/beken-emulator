#include "periph/saradc.h"

#include <algorithm>
#include <utility>

namespace bk {

namespace {

constexpr uint32_t kConfig = 0x00, kData = 0x04, kCtrlCfg = 0x08, kSaturationCfg = 0x0C, kDatAfterSta = 0x10;
constexpr uint32_t kChnlEn = 1u << 2, kIntClr = 1u << 8;
constexpr uint32_t kBusy = 1u << 29, kFifoEmpty = 1u << 30, kFifoFull = 1u << 31;
constexpr uint32_t kStatus       = kBusy | kFifoEmpty | kFifoFull;
constexpr uint32_t kDepth        = 32;
constexpr uint64_t kSampleNs     = 100'000;
constexpr uint32_t kCtrlCfgReset = 0xE0, kDatAfterStaEmpty = 0xFFFF;

} // namespace

Saradc::Saradc(std::string name, Line irq, uint32_t sample)
    : name_(std::move(name)), irq_(std::move(irq)), sample_(sample), ctrl_cfg_(kCtrlCfgReset) {}

uint32_t Saradc::read(const std::string &, uint32_t offset, uint32_t) {
    switch (offset) {
        case kConfig: return config_ | (fifo_ == 0 ? kFifoEmpty : 0) | (fifo_ >= kDepth ? kFifoFull : 0);
        case kData: return pop(0);
        case kDatAfterSta: return pop(kDatAfterStaEmpty);
        case kCtrlCfg: return ctrl_cfg_;
        case kSaturationCfg: return saturation_cfg_;
        default: return 0;
    }
}

void Saradc::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    switch (offset) {
        case kConfig: write_config(value); break;
        case kCtrlCfg: ctrl_cfg_ = value; break;
        case kSaturationCfg: saturation_cfg_ = value; break;
        default: break;
    }
    update_irq();
}

void Saradc::tick(uint64_t now_ns) {
    last_tick_ = now_ns;
    while (next_ns_ && now_ns >= *next_ns_) {
        if (fifo_ < kDepth) {
            ++fifo_;
            if (fifo_ >= std::max<uint32_t>(ctrl_cfg_ & 0x1F, 1)) pending_ = true;
        }
        *next_ns_ += kSampleNs;
    }
    update_irq();
}

void Saradc::write_config(uint32_t value) {
    const bool was_enabled = (config_ & kChnlEn) != 0;
    config_                = value & ~(kStatus | kIntClr);
    if (value & kIntClr) pending_ = false;
    const bool enabled = (value & kChnlEn) != 0;
    if (enabled && !was_enabled) {
        next_ns_ = last_tick_ + kSampleNs;
    } else if (was_enabled && !enabled) {
        next_ns_.reset();
        fifo_ = 0;
    }
}

uint32_t Saradc::pop(uint32_t when_empty) {
    if (fifo_ == 0) return when_empty;
    --fifo_;
    return sample_;
}

void Saradc::update_irq() {
    if (irq_) irq_(pending_);
}

void Saradc::reset() {
    config_         = 0;
    ctrl_cfg_       = kCtrlCfgReset;
    saturation_cfg_ = 0;
    last_tick_      = 0;
    fifo_           = 0;
    next_ns_.reset();
    pending_ = false;
    update_irq();
}

} // namespace bk
