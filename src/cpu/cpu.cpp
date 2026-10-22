#include "cpu/cpu.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace bk {

namespace {

constexpr uint32_t kModeFiq = 0x11, kModeIrq = 0x12, kModeSvc = 0x13, kModeAbt = 0x17, kModeUnd = 0x1B;
constexpr uint32_t kBitI = 0x80, kBitF = 0x40, kBitT = 0x20;

// Interrupt number Unicorn reports for a SWI.
constexpr uint32_t kIntnoSwi = 2;
// Fixed instruction cost charged for every exception taken inside a slice,
// since Unicorn does not report how many instructions actually ran.
constexpr int64_t kExceptionCost = 50;

// MCR p15, 0, r0, c7, c0, 4: wait for interrupt.
constexpr uint32_t kWfiArm     = 0xEE070F90;
constexpr uint16_t kWfiThumbHi = 0xEE07, kWfiThumbLo = 0x0F90;

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

void check(uc_err err, const char *what) {
    if (err != UC_ERR_OK) throw std::runtime_error(std::string(what) + ": " + uc_strerror(err));
}

int uc_reg_id(Reg r) {
    switch (r) {
        case Reg::SP: return UC_ARM_REG_SP;
        case Reg::LR: return UC_ARM_REG_LR;
        case Reg::PC: return UC_ARM_REG_PC;
        case Reg::CPSR: return UC_ARM_REG_CPSR;
        case Reg::SPSR: return UC_ARM_REG_SPSR;
        default: return UC_ARM_REG_R0 + static_cast<int>(r);
    }
}

} // namespace

Cpu::Cpu() {
    check(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_), "uc_open");
    try {
        check(uc_ctl_set_cpu_model(uc_, UC_CPU_ARM_946), "set cpu model");
        check(uc_hook_add(uc_, &intr_hook_, UC_HOOK_INTR, reinterpret_cast<void *>(&Cpu::on_intr), this, 1, 0),
              "add interrupt hook");
    } catch (...) {
        uc_close(uc_);
        throw;
    }
}

Cpu::~Cpu() { uc_close(uc_); }

void Cpu::reset(uint32_t pc) {
    set_reg(Reg::CPSR, kModeSvc | kBitI | kBitF);
    for (int r = static_cast<int>(Reg::R0); r <= static_cast<int>(Reg::LR); ++r) set_reg(static_cast<Reg>(r), 0);
    set_reg(Reg::PC, pc);
    irq_ = fiq_ = false;
    pending_.reset();
    restart_ = false;
}

bool Cpu::thumb() { return (reg(Reg::CPSR) & kBitT) != 0; }

void Cpu::enter(uint32_t mode, uint32_t vector, uint32_t lr, uint32_t mask) {
    const uint32_t old = reg(Reg::CPSR);
    // CPSR first so the core switches to the banked SPSR/LR of the new mode;
    // clearing the low six bits also leaves Thumb state.
    set_reg(Reg::CPSR, (old & ~0x3Fu) | mode | mask);
    set_reg(Reg::SPSR, old);
    set_reg(Reg::LR, lr);
    set_reg(Reg::PC, vector);
}

void Cpu::enter_exception(Exc kind, uint32_t fault_pc) {
    const uint32_t step = thumb() ? 2 : 4;
    switch (kind) {
        case Exc::Undef: enter(kModeUnd, 0x04, fault_pc + step, kBitI); break;
        case Exc::Swi: enter(kModeSvc, 0x08, fault_pc + step, kBitI); break;
        case Exc::Pabt: enter(kModeAbt, 0x0C, fault_pc + 4, kBitI); break;
        case Exc::Dabt: enter(kModeAbt, 0x10, fault_pc + 8, kBitI); break;
        case Exc::Irq: enter(kModeIrq, 0x18, fault_pc + 4, kBitI); break;
        case Exc::Fiq: enter(kModeFiq, 0x1C, fault_pc + 4, kBitI | kBitF); break;
    }
}

bool Cpu::take_pending(uint32_t pc) {
    const uint32_t cpsr = reg(Reg::CPSR);
    if (fiq_ && !(cpsr & kBitF)) {
        enter_exception(Exc::Fiq, pc);
        return true;
    }
    if (irq_ && !(cpsr & kBitI)) {
        enter_exception(Exc::Irq, pc);
        return true;
    }
    return false;
}

bool Cpu::at_wait_for_interrupt() {
    const uint32_t pc = reg(Reg::PC);
    if (pc < 4) return false;
    uint8_t b[4];
    if (uc_mem_read(uc_, pc - 4, b, sizeof b) != UC_ERR_OK) return false;
    if (thumb()) {
        const uint16_t hi = static_cast<uint16_t>(b[0] | b[1] << 8);
        const uint16_t lo = static_cast<uint16_t>(b[2] | b[3] << 8);
        return hi == kWfiThumbHi && lo == kWfiThumbLo;
    }
    const uint32_t word = b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24;
    return word == kWfiArm;
}

void Cpu::on_intr(uc_engine *uc, uint32_t intno, void *user) {
    auto *self = static_cast<Cpu *>(user);
    if (intno == kIntnoSwi) {
        // The reported pc is already past the SWI.
        const uint32_t pc = self->reg(Reg::PC);
        if (!self->pending_) self->pending_ = Pending{Exc::Swi, pc - (self->thumb() ? 2u : 4u)};
    } else if (!self->bad_intno_) {
        self->bad_intno_ = intno;
    }
    uc_emu_stop(uc);
}

std::pair<RunResult, uint32_t> Cpu::run(uint32_t budget) {
    if (stopped_) return {RunResult::Stopped, 0};
    take_pending(reg(Reg::PC));
    // Unicorn treats a count of 0 as "no limit", so an empty budget runs nothing.
    if (budget == 0) return {RunResult::Budget, 0};

    int64_t remaining = budget;
    for (;;) {
        const uint32_t start = reg(Reg::PC) | (thumb() ? 1u : 0u);
        bad_intno_.reset();
        const uc_err err = uc_emu_start(uc_, start, 0xFFFFFFFF, 0, static_cast<size_t>(remaining));
        if (err != UC_ERR_OK && !pending_) {
            if (err == UC_ERR_INSN_INVALID)
                pending_ = Pending{Exc::Undef, reg(Reg::PC)};
            else
                throw std::runtime_error("CPU error at pc " + hex(reg(Reg::PC)) + ": " + uc_strerror(err));
        }
        if (bad_intno_) throw std::runtime_error("unhandled CPU exception " + std::to_string(*bad_intno_));
        if (stopped_) return {RunResult::Stopped, 0};

        if (pending_) {
            const Pending p = *pending_;
            pending_.reset();
            // A SWI yields to a pending, unmasked interrupt: the interrupt returns
            // to the SWI, which then runs again.
            if (p.kind != Exc::Swi || !take_pending(p.pc)) enter_exception(p.kind, p.pc);
            remaining -= kExceptionCost;
            if (remaining <= 0) return {RunResult::Budget, budget};
            continue;
        }

        if (!restart_) break;
        restart_ = false;
        // Blocks translated before a new hook was installed would not call it.
        check(uc_ctl_flush_tb(uc_), "flush translation cache");
    }

    if (at_wait_for_interrupt()) return {RunResult::Idle, 1};
    return {RunResult::Budget, budget};
}

void Cpu::post_fault(Fault::Kind kind) {
    if (!pending_) {
        // Inside a memory callback pc is the instruction making the access.
        pending_ = Pending{kind == Fault::Kind::Dabt ? Exc::Dabt : Exc::Pabt, reg(Reg::PC)};
    }
    uc_emu_stop(uc_);
}

void Cpu::request_restart() {
    restart_ = true;
    uc_emu_stop(uc_);
}

void Cpu::stop() {
    stopped_ = true;
    uc_emu_stop(uc_);
}

void Cpu::map_ram(uint32_t base, uint32_t size, bool executable) {
    check(uc_mem_map(uc_, base, size, executable ? UC_PROT_ALL : UC_PROT_READ | UC_PROT_WRITE), "map ram");
}

void Cpu::mem_write(uint32_t addr, std::span<const uint8_t> data) {
    check(uc_mem_write(uc_, addr, data.data(), data.size()), "memory write");
}

std::vector<uint8_t> Cpu::mem_read(uint32_t addr, uint32_t size) {
    std::vector<uint8_t> out(size);
    check(uc_mem_read(uc_, addr, out.data(), size), "memory read");
    return out;
}

uint32_t Cpu::reg(Reg r) {
    uint32_t v = 0;
    check(uc_reg_read(uc_, uc_reg_id(r), &v), "register read");
    return v;
}

void Cpu::set_reg(Reg r, uint32_t value) { check(uc_reg_write(uc_, uc_reg_id(r), &value), "register write"); }

} // namespace bk
