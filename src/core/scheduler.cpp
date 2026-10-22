#include "core/scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bk {

Scheduler::Scheduler(Cpu &cpu, uint32_t insns_per_us) : cpu_(cpu), insns_per_us_(insns_per_us) {
    if (insns_per_us == 0) throw std::invalid_argument("insns_per_us must be positive");
}

void Scheduler::add(Module *module) { modules_.push_back(module); }

bool Scheduler::step() {
    if (stopped_) return false;
    auto [result, executed] = cpu_.run(kSlice);
    switch (result) {
        case RunResult::Stopped: stopped_ = true; return false;
        case RunResult::Idle: {
            // Events at or before now are ignored, so a module that reports a
            // stale event cannot hold time still.
            std::optional<uint64_t> target;
            for (Module *m : modules_) {
                auto e = m->next_event_ns();
                if (e && *e > now_ && (!target || *e < *target)) target = e;
            }
            now_ = std::max(now_, target ? *target : now_ + slice_ns());
            break;
        }
        case RunResult::Budget: now_ += uint64_t{executed} * 1000 / insns_per_us_; break;
    }
    // A tick may reset the chip (watchdog expiry); module objects survive a
    // reset, so the remaining modules of this step are ticked as usual.
    for (size_t i = 0; i < modules_.size(); ++i) modules_[i]->tick(now_);
    return true;
}

void Scheduler::run_forever(std::function<bool()> should_stop) {
    while (!stopped_ && !(should_stop && should_stop())) step();
}

} // namespace bk
