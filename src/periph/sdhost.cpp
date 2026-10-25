#include "periph/sdhost.h"

namespace bk {

void SdHost::write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) {
    if (size != 4 || (offset != kCmdSendCtrl && offset != kStatus)) {
        Unknown::write(range, offset, size, value);
        return;
    }
    // The status read before the write, so the log line and the stored word
    // are the plain ones and the effect follows.
    const uint32_t status = peek(kStatus);
    Unknown::write(range, offset, size, value);
    if (offset == kStatus)
        poke(kStatus, status & ~value);
    else if (value & kStart)
        poke(kStatus, status | kTimeout);
}

} // namespace bk
