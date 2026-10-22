#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/module.h"
#include "cpu/cpu.h"

namespace bk {

// Owns device time and drives the CPU in slices of 1000 instructions.
//
// Device time starts at 0, is never reset and does not advance inside a slice:
// every register access during a slice sees the time from the start of that
// slice. After each slice every module is ticked, in the order added, with the
// new time.
class Scheduler : public Clock {
  public:
    static constexpr uint32_t kSlice = 1000;

    // Throws std::invalid_argument if insns_per_us is 0.
    Scheduler(Cpu &cpu, uint32_t insns_per_us);

    void add(Module *module);

    // Runs one slice and advances time:
    //  - the CPU stopped: the scheduler is stopped, nothing is ticked;
    //  - the slice ended at a wait for interrupt: time jumps to the earliest
    //    module event later than now, or one slice of time if there is none;
    //  - otherwise time advances by one slice (kSlice instructions).
    // Then ticks every module. Returns false once stopped.
    bool step();
    // Steps until the scheduler stops or should_stop() returns true (checked
    // before every step).
    void run_forever(std::function<bool()> should_stop);

    uint64_t now_ns() const override { return now_; }
    bool     stopped() const { return stopped_; }

  private:
    uint64_t slice_ns() const { return uint64_t{kSlice} * 1000 / insns_per_us_; }

    Cpu                  &cpu_;
    uint32_t              insns_per_us_;
    std::vector<Module *> modules_;
    uint64_t              now_     = 0;
    bool                  stopped_ = false;
};

} // namespace bk
