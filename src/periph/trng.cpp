#include "periph/trng.h"

namespace bk {

namespace {
constexpr uint32_t kCtrl = 0x00, kData = 0x04;
}

uint32_t Trng::read(const std::string &, uint32_t offset, uint32_t) {
    if (offset == kCtrl) return ctrl_;
    if (offset == kData) {
        if (ctrl_ & 1) return count_++;
        return latch_;
    }
    return 0;
}

void Trng::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    if (offset == kCtrl)
        ctrl_ = value & 1;
    else if (offset == kData && !(ctrl_ & 1))
        latch_ = value;
}

void Trng::reset() {
    ctrl_  = 1;
    count_ = 0;
    latch_ = 0;
}

} // namespace bk
