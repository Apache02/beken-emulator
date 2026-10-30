#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/module.h"
#include "periph/gdma_engine.h"

namespace bk {

// The BK7221U general DMA: six channels on the GdmaEngine. Channel n is the
// eight words at n * 0x20 (config, dst_start, src_start, dst loop end and
// start, src loop end and start, req_mux); then status (remain_len and the
// event counts) per channel at 0xC0, prio_mode at 0xDC, int_status at 0xE0
// (finish flags in bits 5:0, half-finish flags in 13:8, write 1 to clear),
// src and dst pause at 0x100 and 0x120, dst_wr_addr at 0x160. Request lines
// are nibbles of req_mux (source 3:0, destination 7:4); 0 is memory.
class Gdma : public Module {
  public:
    static constexpr unsigned kChannels = 6;

    // `irq` may be empty; `bus_hz` is the clock the transfer times were
    // measured at.
    Gdma(std::string name, const Clock &clock, Line irq, BusMemory &memory, uint32_t bus_hz);

    std::string        name() const override { return name_; }
    std::vector<Range> ranges() const override {
        return {
            {"regs", 0x200}
        };
    }
    uint32_t                read(const std::string &range, uint32_t offset, uint32_t size) override;
    void                    write(const std::string &range, uint32_t offset, uint32_t size, uint32_t value) override;
    void                    tick(uint64_t now_ns) override { engine_.tick(now_ns); }
    std::optional<uint64_t> next_event_ns() const override { return engine_.next_event_ns(); }
    void                    reset() override { engine_.reset(); }
    std::shared_ptr<RequestLine> request_line(unsigned n) override { return engine_.request_line(n); }

  private:
    std::string name_;
    GdmaEngine  engine_;
};

} // namespace bk
