#pragma once

#include <unicorn/unicorn.h>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "core/module.h"

namespace bk {

enum class RunResult { Budget, Idle, Stopped };

enum class Reg { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC, CPSR, SPSR };

// ARM9 core built on Unicorn. Unicorn does not perform exception entry the
// way this chip needs, so undefined instructions, SWIs, aborts, IRQ and FIQ
// are entered by hand: the mode switch, SPSR, LR and the jump to the fixed
// low vector are written explicitly.
//
// IRQ/FIQ request levels are sampled only at the start of run(), so the
// interrupt latency is at most one slice.
class Cpu {
  public:
    Cpu();
    ~Cpu();
    Cpu(const Cpu &)            = delete;
    Cpu &operator=(const Cpu &) = delete;

    uc_engine *engine() { return uc_; }

    // SVC mode with I and F set, r0-r12/sp/lr zero, pc = `pc`; clears interrupt
    // requests, pending fault and restart flag. Memory is left untouched.
    void reset(uint32_t pc);

    // Executes up to `budget` instructions. Returns Idle (with 1) when the slice
    // ended on a wait-for-interrupt instruction, Budget (with `budget`)
    // otherwise, Stopped (with 0) after stop(). Throws std::runtime_error when
    // execution fails in a way no exception entry explains.
    std::pair<RunResult, uint32_t> run(uint32_t budget);

    void raise_irq(bool level) { irq_ = level; }
    void raise_fiq(bool level) { fiq_ = level; }

    // Safe to call from inside a Unicorn memory callback: records an abort for
    // the instruction currently executing (unless one is already pending) and
    // stops emulation; run() then enters the abort handler.
    void post_fault(Fault::Kind kind);
    // Safe to call from inside a Unicorn callback: stops emulation and makes
    // run() drop cached translations and continue from the current pc.
    void request_restart();
    // Stops execution for good: every later run() returns Stopped.
    void stop();

    void                 map_ram(uint32_t base, uint32_t size, bool executable);
    void                 mem_write(uint32_t addr, std::span<const uint8_t> data);
    std::vector<uint8_t> mem_read(uint32_t addr, uint32_t size);

    uint32_t reg(Reg r);
    void     set_reg(Reg r, uint32_t value);

  private:
    enum class Exc { Undef, Swi, Pabt, Dabt, Irq, Fiq };
    struct Pending {
        Exc      kind;
        uint32_t pc;
    };

    static void on_intr(uc_engine *uc, uint32_t intno, void *user);
    void        enter(uint32_t mode, uint32_t vector, uint32_t lr, uint32_t mask);
    void        enter_exception(Exc kind, uint32_t fault_pc);
    bool        take_pending(uint32_t pc);
    bool        thumb();
    bool        at_wait_for_interrupt();

    uc_engine              *uc_        = nullptr;
    uc_hook                 intr_hook_ = 0;
    bool                    irq_       = false;
    bool                    fiq_       = false;
    bool                    restart_   = false;
    bool                    stopped_   = false;
    std::optional<Pending>  pending_;
    std::optional<uint32_t> bad_intno_;
};

} // namespace bk
