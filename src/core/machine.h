#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bus/bus.h"
#include "core/endpoint.h"
#include "core/module.h"
#include "core/profile.h"
#include "core/scheduler.h"
#include "cpu/cpu.h"

namespace bk {

// Host ends of UARTs by module name (non-owning).
using EndpointMap = std::map<std::string, ByteEndpoint *>;

// Everything a module factory receives. Collaborators are resolved by the
// machine: `intc`/`irq` become lines, `*_signal` parameters become signals
// (keyed by the parameter name without the suffix). Those parameters are
// removed from `params`; everything else is passed through unchanged, and a
// factory ignores parameters it does not know.
struct ModuleContext {
    std::string           name;
    ParamTable            params;
    const Clock          &clock;
    Line                  cpu_irq, cpu_fiq;
    std::function<void()> reset_chip;
    // `irq` given as an integer.
    Line irq;
    // `irq` given as a table of name -> source bit.
    std::map<std::string, Line>   irqs;
    std::map<std::string, Signal> signals;
    ByteEndpoint                 *endpoint = nullptr;

    // Integer parameter `key`, or `fallback` when absent. Throws
    // std::invalid_argument if present but not an integer.
    int64_t integer(const std::string &key, int64_t fallback) const;
    // Signal `key`, or an empty Signal.
    Signal signal(const std::string &key) const;
};

using ModuleFactory = std::function<std::unique_ptr<Module>(const ModuleContext &)>;
// Module type name -> factory.
using ModuleRegistry = std::map<std::string, ModuleFactory>;

// A chip assembled from a profile.
//
// Built in this order: CPU, bus, scheduler and the CPU's IRQ/FIQ request lines;
// a RAM region per memory entry; every module in profile order (constructed,
// its ranges claimed, added to the scheduler); finally the peripheral window.
class Machine {
  public:
    // Throws std::invalid_argument on an unknown module type, a reference to a
    // module not listed earlier, a missing base, overlapping ranges, `irq`
    // without `intc` or a bad parameter.
    Machine(const Profile &profile, const ModuleRegistry &registry, EndpointMap endpoints = {});
    Machine(const Machine &)            = delete;
    Machine &operator=(const Machine &) = delete;

    Cpu       &cpu() { return cpu_; }
    Bus       &bus() { return bus_; }
    Scheduler &scheduler() { return scheduler_; }
    // The module named `name`, or nullptr.
    Module *module(const std::string &name);
    // The profile's load target names, as given.
    const std::map<std::string, AliasSpec> &aliases() const { return aliases_; }

    // Chip reset: fills every region that has a reset fill byte, resets every
    // module in profile order and resets the CPU to pc 0. Flash contents, device
    // time and UART endpoints are left alone.
    //
    // Must not be called from inside a CPU memory callback (a module's read or
    // write): resetting the CPU is only safe between slices, e.g. from
    // Module::tick.
    void reset();

  private:
    void    add_module(const ModuleSpec &spec, const ModuleRegistry &registry, const EndpointMap &endpoints);
    Module &earlier(const std::string &user, const std::string &owner) const;

    std::vector<MemSpec>                 memory_;
    std::map<std::string, AliasSpec>     aliases_;
    Cpu                                  cpu_;
    Bus                                  bus_;
    Scheduler                            scheduler_;
    Line                                 cpu_irq_, cpu_fiq_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::map<std::string, Module *>      by_name_;
};

} // namespace bk
