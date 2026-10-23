#include "periph/sctrl.h"

#include <algorithm>
#include <utility>

#include "core/signal.h"

namespace bk {

namespace {
constexpr uint32_t kChipId = 0x00, kDeviceId = 0x04;
}

Sctrl::Sctrl(std::string                       name,
             uint32_t                          chip_id,
             uint32_t                          device_id,
             std::map<uint32_t, uint32_t>      reset_words,
             std::map<uint32_t, uint32_t>      warm_reset_words,
             std::map<std::string, SignalSpec> signals,
             std::vector<ReadAdjust>           read_adjust)
    : name_(std::move(name)), chip_id_(chip_id), device_id_(device_id), reset_words_(std::move(reset_words)),
      warm_reset_words_(std::move(warm_reset_words)), words_(reset_words_), signals_(std::move(signals)),
      read_adjust_(std::move(read_adjust)) {}

uint32_t Sctrl::stored(uint32_t offset) const {
    auto it = words_.find(offset);
    return it == words_.end() ? 0 : it->second;
}

uint32_t Sctrl::read(const std::string &, uint32_t offset, uint32_t) {
    if (offset == kChipId) return chip_id_;
    if (offset == kDeviceId) return device_id_;
    uint32_t value = stored(offset);
    for (const ReadAdjust &a : read_adjust_) {
        if (a.offset != offset) continue;
        uint64_t max   = a.width >= 32 ? 0xFFFFFFFFull : ((1ull << a.width) - 1);
        uint64_t field = (value >> a.shift) & max;
        field          = std::min<uint64_t>(field + a.add, max);
        value          = static_cast<uint32_t>((value & ~(max << a.shift)) | (field << a.shift));
    }
    return value;
}

void Sctrl::write(const std::string &, uint32_t offset, uint32_t, uint32_t value) {
    if (offset == kChipId || offset == kDeviceId) return;
    words_[offset] = value;
}

void Sctrl::reset() {
    words_ = reset_words_;
    for (const auto &[offset, value] : warm_reset_words_) words_[offset] = value;
}

Signal Sctrl::signal(const std::string &signal_name) {
    auto it = signals_.find(signal_name);
    if (it == signals_.end()) return Module::signal(signal_name);
    return register_signal([this](uint32_t offset) { return read("regs", offset, 4); }, it->second);
}

} // namespace bk
