#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"
#include "periph/gdma_engine.h"

namespace bk {

// The BK7252N general DMA: eight channels on the GdmaEngine. Global words:
// dev_id and ver_id (read 0), control at 0x08 (bit 2 the priority mode),
// secure and privileged attributes at 0x10 and 0x14, int_status at 0x18 (one
// bit per channel, set while either flag of the channel is; writing 1 clears
// both), interrupt allocation at 0x28. Channel n is 0x40 bytes at
// 0x40 + n * 0x40: config, dst_start, src_start, dst loop end and start, src
// loop end and start, req_mux, src and dst pause, src_rd_addr, dst_wr_addr,
// status (remain_len 16:0, the finish and half-finish flags in bits 18 and 19
// - write 1 to clear - and the event counts in 27:24 and 31:28). Request lines
// are 5-bit fields of req_mux (source 4:0, destination 9:5); 0x00 and 0x10
// are memory.
class GdmaBk7252n : public Module {
  public:
    static constexpr unsigned kChannels = 8;

    // `irq` may be empty; `bus_hz` is the clock the transfer times were
    // measured at.
    GdmaBk7252n(std::string name, const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x40 + kChannels * 0x40}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override { engine_.tick(now_ns); }
    std::optional<uint64_t> next_event_ns() const override { return engine_.next_event_ns(); }
    void                    reset() override;
    std::shared_ptr<RequestLine> request_line(unsigned n) override { return engine_.request_line(n); }

  private:
    std::string name_;
    GdmaEngine  engine_;
    // Words the engine does not use: control (its priority mode bit lives in
    // the engine), the attributes and the interrupt allocation.
    uint32_t control_ = 0, secure_ = 0, privileged_ = 0, allocate_ = 0;
};

} // namespace bk
