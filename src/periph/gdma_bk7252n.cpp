#include "periph/gdma_bk7252n.h"

#include <utility>

namespace bk {

namespace {

constexpr uint32_t kControl = 0x08, kSecure = 0x10, kPrivileged = 0x14, kIntStatus = 0x18, kAllocate = 0x28;
constexpr uint32_t kPrioMode    = 1u << 2;
constexpr uint32_t kChannelBase = 0x40, kChStride = 0x40;
constexpr uint32_t kConfig = 0x00, kDstStart = 0x04, kSrcStart = 0x08, kLoops = 0x0C, kMux = 0x1C;
constexpr uint32_t kSrcPause = 0x20, kDstPause = 0x24, kSrcRd = 0x28, kDstWr = 0x2C, kStatus = 0x30;
constexpr uint32_t kFinFlag = 1u << 18, kHalfFlag = 1u << 19;

// TX lines 0x00-0x09, RX lines 0x10-0x19 (vendor SDK GDMA_X_*_REQ).
const char *request_name(uint32_t request) {
    static const char *const tx[] = {
        "DTCM", "UART1", "UART2", "UART3", "GSPI", "SDIO", "HSSPI", "JPEG", "I2S", "AUDIO"};
    static const char *const rx[] = {
        "DTCM", "UART1_RX", "UART2_RX", "UART3_RX", "GSPI_RX", "SDIO_RX", "HSSPI_RX", "JPEG_RX", "I2S_RX", "AUDIO_RX"};
    const uint32_t line = request & 0xF;
    if (line >= 10 || request >= 0x20) return nullptr;
    return request & 0x10 ? rx[line] : tx[line];
}

const GdmaEngine::Encoding kEncoding{
    0, 5, 5, {0x00, 0x10},
       3, request_name
};

} // namespace

GdmaBk7252n::GdmaBk7252n(std::string name, const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz)
    : name_(std::move(name)), engine_(clock, std::move(irq), memory, bus_hz, kChannels, kEncoding) {}

void GdmaBk7252n::reset() {
    engine_.reset();
    control_ = secure_ = privileged_ = allocate_ = 0;
}

uint32_t GdmaBk7252n::read(const std::string &, uint32_t offset, uint32_t) {
    engine_.sync();
    if (offset >= kChannelBase) {
        const unsigned index  = (offset - kChannelBase) / kChStride;
        const uint32_t within = (offset - kChannelBase) % kChStride;
        if (index >= kChannels) return 0;
        const GdmaEngine::Words &w = engine_.words(index);
        switch (within) {
            case kConfig: return engine_.config(index);
            case kDstStart: return w.dst;
            case kSrcStart: return w.src;
            case kMux: return w.mux;
            case kSrcPause: return w.pause[0];
            case kDstPause: return w.pause[1];
            case kSrcRd: return engine_.src_rd_addr(index);
            case kDstWr: return engine_.dst_wr_addr(index);
            case kStatus:
                return (engine_.remain(index) & 0x1FFFF) | ((engine_.finished() >> index & 1) ? kFinFlag : 0) |
                       ((engine_.half_finished() >> index & 1) ? kHalfFlag : 0) | engine_.fin_count(index) << 24 |
                       engine_.half_count(index) << 28;
            default: return within < kMux ? w.loops[(within - kLoops) / 4] : 0;
        }
    }
    switch (offset) {
        case kControl: return control_ | (engine_.priority_mode() ? kPrioMode : 0);
        case kSecure: return secure_;
        case kPrivileged: return privileged_;
        case kIntStatus: return engine_.finished() | engine_.half_finished();
        case kAllocate: return allocate_;
        default: return 0;
    }
}

void GdmaBk7252n::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    engine_.sync();
    if (offset >= kChannelBase) {
        const unsigned index  = (offset - kChannelBase) / kChStride;
        const uint32_t within = (offset - kChannelBase) % kChStride;
        if (index < kChannels) {
            GdmaEngine::Words &w = engine_.words(index);
            switch (within) {
                case kConfig: engine_.write_config(index, value); break;
                case kDstStart: w.dst = value; break;
                case kSrcStart: w.src = value; break;
                case kMux: w.mux = value; break;
                case kSrcPause: w.pause[0] = value; break;
                case kDstPause: w.pause[1] = value; break;
                case kStatus:
                    engine_.acknowledge((value & kFinFlag) ? 1u << index : 0, (value & kHalfFlag) ? 1u << index : 0);
                    break;
                default:
                    if (within < kMux) w.loops[(within - kLoops) / 4] = value;
                    break;
            }
        }
    } else {
        switch (offset) {
            case kControl:
                control_ = value & ~kPrioMode;
                engine_.set_priority_mode(value & kPrioMode);
                break;
            case kSecure: secure_ = value; break;
            case kPrivileged: privileged_ = value; break;
            case kIntStatus: engine_.acknowledge(value, value); break;
            case kAllocate: allocate_ = value; break;
            default: break;
        }
    }
    engine_.settle();
}

} // namespace bk
