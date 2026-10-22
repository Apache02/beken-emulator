#include <doctest/doctest.h>

#include <functional>
#include <stdexcept>
#include <string>

#include "cpu/cpu.h"

using namespace bk;

namespace {

// Modes and CPSR bits.
constexpr uint32_t kUsr = 0x10, kFiq = 0x11, kIrq = 0x12, kSvc = 0x13, kAbt = 0x17, kUnd = 0x1B, kSys = 0x1F;
constexpr uint32_t kI = 0x80, kF = 0x40, kT = 0x20;

// ARM encodings.
constexpr uint32_t kSwi      = 0xEF000000; // swi 0
constexpr uint32_t kUdf      = 0xE7F000F0; // udf #0
constexpr uint32_t kWfi      = 0xEE070F90; // mcr p15, 0, r0, c7, c0, 4
constexpr uint32_t kWfiR1    = 0xEE071F90; // mcr p15, 0, r1, c7, c0, 4
constexpr uint32_t kBkpt     = 0xE1200070; // bkpt 0
constexpr uint32_t kLdrR1R2  = 0xE5921000; // ldr r1, [r2]
constexpr uint32_t kAddR0One = 0xE2800001; // add r0, r0, #1
constexpr uint32_t mov_r0(uint8_t imm) { return 0xE3A00000 | imm; }
constexpr uint32_t b(uint32_t from, uint32_t to) { return 0xEA000000 | (((to - from - 8) >> 2) & 0x00FFFFFF); }

// Thumb encodings.
constexpr uint16_t kTSvc = 0xDF00; // svc 0
constexpr uint16_t kTUdf = 0xDE00; // udf #0
constexpr uint16_t t_b(uint32_t from, uint32_t to) {
    return static_cast<uint16_t>(0xE000 | (((to - from - 4) >> 1) & 0x7FF));
}

using CodeFn = std::function<void(uint64_t addr)>;

void code_trampoline(uc_engine *, uint64_t addr, uint32_t, void *user) { (*static_cast<CodeFn *>(user))(addr); }

using MemFn = std::function<bool(uint64_t addr)>;

bool mem_trampoline(uc_engine *, uc_mem_type, uint64_t addr, int, int64_t, void *user) {
    return (*static_cast<MemFn *>(user))(addr);
}

struct Fixture {
    Cpu    cpu;
    CodeFn on_code = [](uint64_t) {};
    MemFn  on_mem  = [](uint64_t) { return false; };

    Fixture() {
        cpu.map_ram(0, 0x10000, true);
        // Vector table: every vector branches to its own parking loop.
        for (uint32_t v = 0; v < 0x20; v += 4) {
            put(v, b(v, handler(v)));
            put(handler(v), b(handler(v), handler(v)));
        }
        cpu.reset(0x100);
    }

    static uint32_t handler(uint32_t vector) { return 0x1000 + vector * 0x40; }

    void put(uint32_t addr, uint32_t word) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(word),
                                  static_cast<uint8_t>(word >> 8),
                                  static_cast<uint8_t>(word >> 16),
                                  static_cast<uint8_t>(word >> 24)};
        cpu.mem_write(addr, bytes);
    }
    void put16(uint32_t addr, uint16_t half) {
        const uint8_t bytes[2] = {static_cast<uint8_t>(half), static_cast<uint8_t>(half >> 8)};
        cpu.mem_write(addr, bytes);
    }

    void hook_code() {
        uc_hook h;
        REQUIRE(
            uc_hook_add(cpu.engine(), &h, UC_HOOK_CODE, reinterpret_cast<void *>(code_trampoline), &on_code, 1, 0) ==
            UC_ERR_OK);
    }
    void hook_mem(int type) {
        uc_hook h;
        REQUIRE(uc_hook_add(cpu.engine(), &h, type, reinterpret_cast<void *>(mem_trampoline), &on_mem, 1, 0) ==
                UC_ERR_OK);
    }

    uint32_t cpsr() { return cpu.reg(Reg::CPSR); }
    uint32_t pc() { return cpu.reg(Reg::PC); }
};

using Result = std::pair<RunResult, uint32_t>;

} // namespace

TEST_CASE("reset state") {
    Fixture f;
    f.cpu.set_reg(Reg::R3, 7);
    f.cpu.set_reg(Reg::SP, 0x8000);
    f.cpu.set_reg(Reg::LR, 0x1234);
    f.cpu.set_reg(Reg::CPSR, kSys);
    f.cpu.reset(0);
    CHECK(f.cpsr() == (kSvc | kI | kF));
    CHECK(f.pc() == 0);
    CHECK(f.cpu.reg(Reg::R3) == 0);
    CHECK(f.cpu.reg(Reg::SP) == 0);
    CHECK(f.cpu.reg(Reg::LR) == 0);
}

TEST_CASE("runs a slice of code") {
    Fixture f;
    f.put(0x100, b(0x100, 0x100));
    int executed = 0;
    f.on_code    = [&](uint64_t) { ++executed; };
    f.hook_code();
    CHECK(f.cpu.run(1000) == Result{RunResult::Budget, 1000});
    CHECK(executed == 1000);
    CHECK(f.pc() == 0x100);
}

TEST_CASE("a budget of 1 runs one instruction") {
    Fixture f;
    f.put(0x100, mov_r0(1));
    f.put(0x104, mov_r0(2));
    CHECK(f.cpu.run(1) == Result{RunResult::Budget, 1});
    CHECK(f.cpu.reg(Reg::R0) == 1);
    CHECK(f.pc() == 0x104);
}

TEST_CASE("a budget of 0 runs nothing") {
    Fixture f;
    f.put(0x100, mov_r0(1));
    CHECK(f.cpu.run(0) == Result{RunResult::Budget, 0});
    CHECK(f.cpu.reg(Reg::R0) == 0);
    CHECK(f.pc() == 0x100);
}

TEST_CASE("swi enters svc vector") {
    Fixture f;
    f.put(0x100, kSwi);
    f.cpu.set_reg(Reg::CPSR, kUsr);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpsr() == (kSvc | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kUsr);
    CHECK(f.cpu.reg(Reg::LR) == 0x104);
    CHECK(f.pc() == Fixture::handler(0x08));
}

TEST_CASE("thumb swi lr offset 2") {
    Fixture f;
    f.put16(0x100, kTSvc);
    f.cpu.set_reg(Reg::CPSR, kSys | kT);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpsr() == (kSvc | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == (kSys | kT));
    CHECK(f.cpu.reg(Reg::LR) == 0x102);
    CHECK(f.pc() == Fixture::handler(0x08));
}

TEST_CASE("undefined instruction enters und") {
    Fixture f;
    f.put(0x100, mov_r0(9));
    f.put(0x104, kUdf);
    f.cpu.set_reg(Reg::CPSR, kSys);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpu.reg(Reg::R0) == 9);
    CHECK(f.cpsr() == (kUnd | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x108);
    CHECK(f.pc() == Fixture::handler(0x04));
}

TEST_CASE("thumb undefined instruction lr offset 2") {
    Fixture f;
    f.put16(0x100, kTUdf);
    f.cpu.set_reg(Reg::CPSR, kSys | kT);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpsr() == (kUnd | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == (kSys | kT));
    CHECK(f.cpu.reg(Reg::LR) == 0x102);
    CHECK(f.pc() == Fixture::handler(0x04));
}

TEST_CASE("masked irq not taken") {
    Fixture f;
    f.put(0x100, b(0x100, 0x100));
    f.cpu.set_reg(Reg::CPSR, kSys | kI);
    f.cpu.raise_irq(true);
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kSys | kI));
    CHECK(f.pc() == 0x100);
}

TEST_CASE("irq taken at slice start only when I clear") {
    Fixture f;
    f.put(0x100, mov_r0(1));
    f.put(0x104, mov_r0(2));
    f.put(0x108, b(0x108, 0x108));
    // Installed before the first run: blocks translated earlier would not call it.
    bool raise_at_104 = false;
    f.on_code         = [&](uint64_t addr) {
        if (addr == 0x104 && raise_at_104) f.cpu.raise_irq(true);
    };
    f.hook_code();
    f.cpu.set_reg(Reg::CPSR, kSys | kI);
    f.cpu.raise_irq(true);
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kSys | kI));
    CHECK(f.pc() == 0x108);

    // Raised in the middle of a slice: noticed only when the next slice starts.
    f.cpu.reset(0x100);
    f.cpu.set_reg(Reg::CPSR, kSys);
    raise_at_104 = true;
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpu.reg(Reg::R0) == 2);
    CHECK(f.cpsr() == kSys);
    CHECK(f.pc() == 0x108);

    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kIrq | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x10C);
    CHECK(f.pc() == Fixture::handler(0x18));
}

TEST_CASE("irq taken before swi and swi reruns") {
    Fixture f;
    f.put(0x100, kSwi);
    f.cpu.set_reg(Reg::CPSR, kSys);
    bool raised = false;
    f.on_code   = [&](uint64_t addr) {
        if (addr == 0x100 && !raised) {
            raised = true;
            f.cpu.raise_irq(true);
        }
    };
    f.hook_code();
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpsr() == (kIrq | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x104);
    CHECK(f.pc() == Fixture::handler(0x18));

    // Return from the interrupt the way a handler would (subs pc, lr, #4).
    f.cpu.raise_irq(false);
    const uint32_t ret = f.cpu.reg(Reg::LR) - 4;
    f.cpu.set_reg(Reg::CPSR, f.cpu.reg(Reg::SPSR));
    f.cpu.set_reg(Reg::PC, ret);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpsr() == (kSvc | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x104);
    CHECK(f.pc() == Fixture::handler(0x08));
}

TEST_CASE("fiq masks both") {
    Fixture f;
    f.put(0x100, b(0x100, 0x100));
    f.cpu.set_reg(Reg::CPSR, kSys);
    f.cpu.raise_fiq(true);
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kFiq | kI | kF));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x104);
    CHECK(f.pc() == Fixture::handler(0x1C));

    // Neither request can preempt the FIQ handler.
    f.cpu.raise_irq(true);
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kFiq | kI | kF));
    CHECK(f.cpu.reg(Reg::LR) == 0x104);
    CHECK(f.pc() == Fixture::handler(0x1C));
}

TEST_CASE("fiq takes priority over irq") {
    Fixture f;
    f.put(0x100, b(0x100, 0x100));
    f.cpu.set_reg(Reg::CPSR, kSys);
    f.cpu.raise_irq(true);
    f.cpu.raise_fiq(true);
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kFiq | kI | kF));
    CHECK(f.pc() == Fixture::handler(0x1C));
}

TEST_CASE("fiq masked by F falls back to irq") {
    Fixture f;
    f.put(0x100, b(0x100, 0x100));
    f.cpu.set_reg(Reg::CPSR, kSys | kF);
    f.cpu.raise_irq(true);
    f.cpu.raise_fiq(true);
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(f.cpsr() == (kIrq | kI | kF));
    CHECK(f.cpu.reg(Reg::SPSR) == (kSys | kF));
    CHECK(f.pc() == Fixture::handler(0x18));
}

TEST_CASE("wfi returns Idle") {
    Fixture f;
    f.put(0x100, kWfi);
    f.put(0x104, mov_r0(42));
    f.put(0x108, b(0x108, 0x108));
    CHECK(f.cpu.run(1000) == Result{RunResult::Idle, 1});
    CHECK(f.pc() == 0x104);
    CHECK(f.cpu.reg(Reg::R0) == 0);
    CHECK(f.cpu.run(1000) == Result{RunResult::Budget, 1000});
    CHECK(f.cpu.reg(Reg::R0) == 42);
    CHECK(f.pc() == 0x108);
}

TEST_CASE("wfi recognised only in the r0 form") {
    Fixture f;
    f.put(0x100, kWfiR1);
    f.put(0x104, b(0x104, 0x104));
    // Whatever the core does with the r1 form, the slice never reports Idle.
    const auto [result, executed] = f.cpu.run(1);
    CHECK(result == RunResult::Budget);
    CHECK(executed == 1);
}

TEST_CASE("wfi recognised by the thumb halfword pair") {
    Fixture f;
    // A Thumb branch over the pair lands right after it.
    f.put16(0x100, t_b(0x100, 0x108));
    f.put16(0x104, 0xEE07);
    f.put16(0x106, 0x0F90);
    f.put16(0x108, t_b(0x108, 0x108));
    f.cpu.set_reg(Reg::CPSR, kSvc | kI | kF | kT);
    CHECK(f.cpu.run(1) == Result{RunResult::Idle, 1});
    CHECK(f.pc() == 0x108);
}

TEST_CASE("exception costs 50 instructions") {
    Fixture f;
    f.put(0x100, kSwi);
    const uint32_t h = Fixture::handler(0x08);
    f.put(h, kAddR0One);
    f.put(h + 4, b(h + 4, h));
    f.cpu.set_reg(Reg::CPSR, kSys);
    int executed = 0;
    f.on_code    = [&](uint64_t) { ++executed; };
    f.hook_code();
    CHECK(f.cpu.run(60) == Result{RunResult::Budget, 60});
    // The swi, then 10 more: the branch at the vector and 9 in the loop.
    CHECK(executed == 11);
    CHECK(f.cpu.reg(Reg::R0) == 5);
    CHECK(f.pc() == h + 4);
}

TEST_CASE("exception using up the budget ends the slice at the vector") {
    Fixture f;
    f.put(0x100, kSwi);
    f.cpu.set_reg(Reg::CPSR, kSys);
    CHECK(f.cpu.run(50) == Result{RunResult::Budget, 50});
    CHECK(f.cpsr() == (kSvc | kI));
    CHECK(f.pc() == 0x08);
}

TEST_CASE("post_fault dabt enters abt with lr pc+8") {
    Fixture f;
    f.put(0x100, kLdrR1R2);
    f.cpu.set_reg(Reg::R2, 0x80000);
    f.cpu.set_reg(Reg::CPSR, kSys);
    int calls = 0;
    f.on_mem  = [&](uint64_t addr) {
        ++calls;
        CHECK(addr == 0x80000);
        f.cpu.post_fault(Fault::Kind::Dabt);
        f.cpu.post_fault(Fault::Kind::Pabt); // ignored: a fault is already pending
        return false;
    };
    f.hook_mem(UC_HOOK_MEM_READ_UNMAPPED);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(calls == 1);
    CHECK(f.cpsr() == (kAbt | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x108);
    CHECK(f.pc() == Fixture::handler(0x10));
}

TEST_CASE("post_fault pabt enters abt with lr pc+4") {
    Fixture f;
    f.put(0x100, b(0x100, 0x80000));
    f.cpu.set_reg(Reg::CPSR, kSys);
    f.on_mem = [&](uint64_t) {
        f.cpu.post_fault(Fault::Kind::Pabt);
        return false;
    };
    f.hook_mem(UC_HOOK_MEM_FETCH_UNMAPPED);
    CHECK(f.cpu.run(100) == Result{RunResult::Budget, 100});
    CHECK(f.cpsr() == (kAbt | kI));
    CHECK(f.cpu.reg(Reg::SPSR) == kSys);
    CHECK(f.cpu.reg(Reg::LR) == 0x80004);
    CHECK(f.pc() == Fixture::handler(0x0C));
}

TEST_CASE("restart flag re-runs the instruction") {
    Fixture f;
    f.put(0x100, mov_r0(42));
    f.put(0x104, b(0x104, 0x104));
    int seen  = 0;
    f.on_code = [&](uint64_t addr) {
        if (addr == 0x100 && ++seen == 1) f.cpu.request_restart();
    };
    f.hook_code();
    CHECK(f.cpu.run(10) == Result{RunResult::Budget, 10});
    CHECK(seen == 2);
    CHECK(f.cpu.reg(Reg::R0) == 42);
    CHECK(f.pc() == 0x104);
}

TEST_CASE("unhandled interrupt number fails") {
    Fixture f;
    f.put(0x100, kBkpt);
    f.cpu.set_reg(Reg::CPSR, kSys);
    try {
        f.cpu.run(10);
        FAIL("no exception");
    } catch (const std::runtime_error &e) {
        CHECK(std::string(e.what()).find("unhandled CPU exception") != std::string::npos);
    }
}

TEST_CASE("non-executable map_ram fetch fails") {
    Fixture f;
    f.cpu.map_ram(0x20000, 0x1000, false);
    f.put(0x20000, mov_r0(1));
    f.cpu.set_reg(Reg::PC, 0x20000);
    CHECK_THROWS_AS(f.cpu.run(10), std::runtime_error);
    CHECK(f.cpu.reg(Reg::R0) == 0);
}

TEST_CASE("mem_read and mem_write round trip") {
    Fixture f;
    f.cpu.map_ram(0x20000, 0x1000, false);
    const uint8_t data[5] = {1, 2, 3, 4, 5};
    f.cpu.mem_write(0x20FFB, data);
    CHECK(f.cpu.mem_read(0x20FFB, 5) == std::vector<uint8_t>{1, 2, 3, 4, 5});
    CHECK(f.cpu.mem_read(0x20000, 2) == std::vector<uint8_t>{0, 0});
    CHECK_THROWS_AS(f.cpu.mem_read(0x30000, 4), std::runtime_error);
}

TEST_CASE("stop ends every later run") {
    Fixture f;
    f.put(0x100, mov_r0(1));
    f.put(0x104, mov_r0(2));
    f.put(0x108, b(0x108, 0x108));
    f.on_code = [&](uint64_t addr) {
        if (addr == 0x104) f.cpu.stop();
    };
    f.hook_code();
    CHECK(f.cpu.run(10) == Result{RunResult::Stopped, 0});
    CHECK(f.cpu.reg(Reg::R0) == 1);
    CHECK(f.cpu.run(10) == Result{RunResult::Stopped, 0});
    CHECK(f.cpu.reg(Reg::R0) == 1);
}
