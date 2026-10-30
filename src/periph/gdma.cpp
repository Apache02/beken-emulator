#include "periph/gdma.h"

#include <utility>

namespace bk {

namespace {

constexpr uint32_t kChStride = 0x20;
constexpr uint32_t kStatus = 0xC0, kPrioMode = 0xDC, kIntStatus = 0xE0;
constexpr uint32_t kSrcPause = 0x100, kDstPause = 0x120, kDstWr = 0x160;
constexpr uint32_t kConfig = 0x00, kDstStart = 0x04, kSrcStart = 0x08, kLoops = 0x0C, kMux = 0x1C;
// int_status reads these reserved bits while a channel is stuck at fixed priority 0.
constexpr uint32_t kStuckBits = 0x00050000;

const char *request_name(uint32_t request) {
    static const char *const names[] = {
        "DTCM", "HSSPI", "AUDIO", "SDIO", "UART1", "UART2", "I2S", "GSPI", "JPEG", "PSRAM_V", "PSRAM_A", "reserved"};
    return request < 12 ? names[request] : nullptr;
}

const GdmaEngine::Encoding kEncoding{0, 4, 4, {0}, 2, request_name};

// Index of the per-channel word at `offset` in the array that starts at `base`.
bool in_array(uint32_t offset, uint32_t base, unsigned &index) {
    if (offset < base || offset >= base + 4 * Gdma::kChannels) return false;
    index = (offset - base) / 4;
    return true;
}

} // namespace

Gdma::Gdma(std::string name, const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz)
    : name_(std::move(name)), engine_(clock, std::move(irq), memory, bus_hz, kChannels, kEncoding) {}

uint32_t Gdma::read(const std::string &, uint32_t offset, uint32_t) {
    engine_.sync();
    unsigned index = offset / kChStride;
    if (index < kChannels) {
        const uint32_t           within = offset % kChStride;
        const GdmaEngine::Words &w      = engine_.words(index);
        switch (within) {
            case kConfig: return engine_.config(index);
            case kDstStart: return w.dst;
            case kSrcStart: return w.src;
            case kMux: return w.mux;
            default: return w.loops[(within - kLoops) / 4];
        }
    }
    if (in_array(offset, kStatus, index))
        return (engine_.remain(index) & 0x1FFFF) | engine_.fin_count(index) << 24 | engine_.half_count(index) << 28;
    if (offset == kPrioMode) return engine_.priority_mode() ? 1 : 0;
    if (offset == kIntStatus)
        return engine_.finished() | engine_.half_finished() << 8 | (engine_.stuck() ? kStuckBits : 0);
    if (in_array(offset, kSrcPause, index)) return engine_.words(index).pause[0];
    if (in_array(offset, kDstPause, index)) return engine_.words(index).pause[1];
    if (in_array(offset, kDstWr, index)) return engine_.dst_wr_addr(index);
    return 0;
}

void Gdma::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    engine_.sync();
    unsigned index = offset / kChStride;
    if (index < kChannels) {
        const uint32_t     within = offset % kChStride;
        GdmaEngine::Words &w      = engine_.words(index);
        switch (within) {
            case kConfig: engine_.write_config(index, value); break;
            case kDstStart: w.dst = value; break;
            case kSrcStart: w.src = value; break;
            case kMux: w.mux = value; break;
            default: w.loops[(within - kLoops) / 4] = value; break;
        }
    } else if (offset == kPrioMode) {
        engine_.set_priority_mode(value & 1);
    } else if (offset == kIntStatus) {
        engine_.acknowledge(value & 0x3F, (value >> 8) & 0x3F);
    } else if (in_array(offset, kSrcPause, index)) {
        engine_.words(index).pause[0] = value;
    } else if (in_array(offset, kDstPause, index)) {
        engine_.words(index).pause[1] = value;
    }
    engine_.settle();
}

} // namespace bk
