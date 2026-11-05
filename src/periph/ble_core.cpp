#include "periph/ble_core.h"

#include <algorithm>
#include <utility>

namespace bk {

namespace {

constexpr uint32_t kRwblecntl = 0x00, kBasetimecnt = 0x1C, kFinetimecnt = 0x20;
constexpr uint32_t kSamp = 1u << 31, kBaseMask = 0x07FFFFFF;
// MASTER_SOFT_RST, MASTER_TGSOFT_RST, REG_SOFT_RST and SWINT_REQ.
constexpr uint32_t kRwblecntlSelfClear = 0xF0000000;
constexpr uint64_t kSlotUs             = 625;

} // namespace

BleCore::BleCore(std::string name, const Clock &clock, uint32_t em_size)
    : name_(std::move(name)), clock_(clock), em_size_(em_size), regs_(kRegsSize / 4), em_(em_size) {}

void BleCore::reset() {
    std::fill(regs_.begin(), regs_.end(), 0);
    std::fill(em_.begin(), em_.end(), 0);
}

uint32_t BleCore::read(const std::string &range, uint32_t offset, uint32_t) {
    if (range == "em") {
        // The bus reads whole words and picks the bytes itself.
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i)
            if (offset + i < em_.size()) value |= static_cast<uint32_t>(em_[offset + i]) << (8 * i);
        return value;
    }
    return offset / 4 < regs_.size() ? regs_[offset / 4] : 0;
}

void BleCore::write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) {
    if (range == "em") {
        for (uint32_t i = 0; i < size; ++i)
            if (offset + i < em_.size()) em_[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        return;
    }
    if (offset / 4 >= regs_.size()) return;
    switch (offset) {
        case kRwblecntl: regs_[offset / 4] = value & ~kRwblecntlSelfClear; break;
        case kBasetimecnt:
            if (value & kSamp) {
                const uint64_t us       = clock_.now_ns() / 1000;
                regs_[kBasetimecnt / 4] = static_cast<uint32_t>(us / kSlotUs) & kBaseMask;
                regs_[kFinetimecnt / 4] = static_cast<uint32_t>(kSlotUs - 1 - us % kSlotUs);
            }
            break;
        default: regs_[offset / 4] = value; break;
    }
}

} // namespace bk
