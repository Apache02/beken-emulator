#include "periph/efuse.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "core/log.h"

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
    const uint32_t addr = (value >> kAddrShift) & kAddrMask;
    uint8_t       &fuse = fuses_[addr];
    if (value & kOperDir) {
        const uint8_t data = static_cast<uint8_t>((value >> kDataShift) & kDataMask);
        const uint8_t was  = fuse;
        fuse |= data;
        char buf[96];
        std::snprintf(buf, sizeof buf, "eFuse byte %u burnt with 0x%02X: 0x%02X -> 0x%02X", addr, data, was, fuse);
        Logger::get("efuse").log(Level::Warning, std::string(buf) + (site_ ? by_site(site_()) : ""));
    } else {
        optr_ = kRdDataValid | fuse;
    }
}

void Efuse::reset() { ctrl_ = optr_ = 0; }

void Efuse::preset(std::span<const uint8_t> bytes) {
    if (bytes.size() > kBytes)
        throw std::invalid_argument("the eFuse has 32 bytes, not " + std::to_string(bytes.size()));
    fuses_.fill(0);
    std::copy(bytes.begin(), bytes.end(), fuses_.begin());
}

} // namespace bk
