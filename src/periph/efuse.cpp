#include "periph/efuse.h"

namespace bk {

namespace {
// efuse_ctrl: oper_en, oper_dir (1 writes), address [12:8], write data [23:16].
constexpr uint32_t kOperEn = 1u << 0, kOperDir = 1u << 1;
constexpr uint32_t kAddrShift = 8, kAddrMask = 0x1F, kDataShift = 16, kDataMask = 0xFF;
// efuse_optr: read data [7:0] and its valid bit.
constexpr uint32_t kRdDataValid = 1u << 8;
} // namespace

uint32_t Efuse::read(uint32_t offset) const { return offset == kCtrl ? ctrl_ : optr_; }

void Efuse::write(uint32_t offset, uint32_t value) {
    if (offset != kCtrl) return;
    ctrl_ = value & ~kOperEn;
    if (!(value & kOperEn)) return;
    uint8_t &fuse = fuses_[(value >> kAddrShift) & kAddrMask];
    if (value & kOperDir)
        fuse |= static_cast<uint8_t>((value >> kDataShift) & kDataMask);
    else
        optr_ = kRdDataValid | fuse;
}

void Efuse::reset() { ctrl_ = optr_ = 0; }

} // namespace bk
