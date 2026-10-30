#include <doctest/doctest.h>

#include <string>

#include "app/registry.h"
#include "app/stop_report.h"
#include "core/machine.h"

using namespace bk;

namespace {

Profile profile() {
    Profile p;
    p.name         = "T";
    p.insns_per_us = 100;
    p.win_base     = 0x00800000;
    p.win_size     = 0x00100000;
    p.memory.push_back({"RAM", 0x400000, 0x40000, uint8_t{0}, false, 0x40000});
    p.modules.push_back({"intc", "ICU", {{"regs", 0x802000}}, {}});
    return p;
}

} // namespace

TEST_CASE("stop_report shows the cpu, the stack and the interrupt controllers") {
    Profile p = profile();
    Machine machine(p, builtin_registry());
    Cpu    &cpu = machine.cpu();
    cpu.reset(0x400000);
    cpu.set_reg(Reg::CPSR, 0xF3); // SVC, Thumb, IRQ and FIQ masked
    cpu.set_reg(Reg::R0, 0x11);
    cpu.set_reg(Reg::LR, 0x0009C5F1);
    cpu.set_reg(Reg::SP, 0x400100);
    const uint8_t word[4] = {0xEF, 0xBE, 0xAD, 0xDE};
    cpu.mem_write(0x400100, word);
    Module *icu = machine.module("ICU");
    icu->write("regs", 0x40, 4, 0x00000101);
    icu->write("regs", 0x44, 4, 1);

    const std::string r = stop_report(p, machine);
    CHECK(r.starts_with("\nmachine state at stop, device time 0.000000 s\n"));
    CHECK(r.find("  pc    0x00400000") != std::string::npos);
    CHECK(r.find("  cpsr  0x000000F3  SVC, Thumb, IRQ masked, FIQ masked\n") != std::string::npos);
    CHECK(r.find("  lines IRQ low, FIQ low\n") != std::string::npos);
    CHECK(r.find("  r0   0x00000011") != std::string::npos);
    CHECK(r.find("  lr   0x0009C5F1") != std::string::npos);
    CHECK(r.find("stack at sp 0x00400100\n  0xDEADBEEF 0x00000000") != std::string::npos);
    CHECK(r.find("interrupt controller ICU\n  global IRQ on, FIQ off\n  enabled 0,8\n  raised  none\n"
                 "  pending none\n") != std::string::npos);
    CHECK(r.ends_with("\n"));
}

TEST_CASE("stop_report says when the stack is not readable") {
    Profile p = profile();
    Machine machine(p, builtin_registry());
    machine.cpu().reset(0x400000);
    machine.cpu().set_reg(Reg::SP, 0x10000000);
    CHECK(stop_report(p, machine).find("stack at sp 0x10000000\n  (not readable)\n") != std::string::npos);
}
