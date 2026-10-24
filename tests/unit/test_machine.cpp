#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/registry.h"
#include "core/log.h"
#include "core/machine.h"
#include "core/profile.h"
#include "core/scheduler.h"
#include "periph/flash.h"

using namespace bk;

namespace {

constexpr uint32_t kWfi = 0xEE070F90; // mcr p15, 0, r0, c7, c0, 4

// Addresses of the test chip.
constexpr uint32_t kCode = 0x00000000, kRam = 0x00400000, kRamSize = 0x10000;
constexpr uint32_t kWin = 0x00800000, kWinSize = 0x00100000;
constexpr uint32_t kUart = 0x00802100, kIcu = 0x00802000, kWdt = 0x00802900, kTimer = 0x00802A00,
                   kFlashCtrl = 0x00803000, kXip = 0x10000000;

constexpr uint32_t ldr(unsigned rt, unsigned rn, uint32_t imm) { return 0xE5900000 | rn << 16 | rt << 12 | imm; }
constexpr uint32_t str(unsigned rt, unsigned rn, uint32_t imm) { return 0xE5800000 | rn << 16 | rt << 12 | imm; }
constexpr uint32_t mov(unsigned rd, uint8_t imm) { return 0xE3A00000 | rd << 12 | imm; }

// A tiny ARM assembler: code from address 0, literal pool from 0x800.
struct Asm {
    uint32_t                                   pc = 0, pool = 0x800;
    std::vector<std::pair<uint32_t, uint32_t>> words; // (address, word)

    void emit(uint32_t w) {
        words.emplace_back(pc, w);
        pc += 4;
    }
    // ldr rd, =value
    void ldr_const(unsigned rd, uint32_t value) {
        uint32_t lit = pool;
        pool += 4;
        words.emplace_back(lit, value);
        emit(0xE59F0000 | rd << 12 | (lit - pc - 8));
    }
    // b to
    void b(uint32_t to) { emit(0xEA000000 | (((to - pc - 8) >> 2) & 0x00FFFFFF)); }
    // A store of `value` to `addr` (uses r0 and r1).
    void poke(uint32_t addr, uint32_t value) {
        ldr_const(1, addr);
        ldr_const(0, value);
        emit(str(0, 1, 0));
    }
    void load(Machine &m) const {
        for (auto [addr, w] : words) {
            const uint8_t bytes[4] = {static_cast<uint8_t>(w),
                                      static_cast<uint8_t>(w >> 8),
                                      static_cast<uint8_t>(w >> 16),
                                      static_cast<uint8_t>(w >> 24)};
            m.bus().write_memory(addr, bytes);
        }
    }
};

uint32_t word_at(Machine &m, uint32_t addr) {
    auto b = m.cpu().mem_read(addr, 4);
    return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24;
}

struct Sink : ByteEndpoint {
    std::vector<uint8_t> out;
    void write(std::span<const uint8_t> data) override { out.insert(out.end(), data.begin(), data.end()); }
    std::vector<uint8_t> read_nowait() override { return {}; }
    std::string          description() const override { return "sink"; }
};

Profile chip() {
    Profile p;
    p.name         = "TEST";
    p.insns_per_us = 100;
    p.win_base     = kWin;
    p.win_size     = kWinSize;
    p.memory       = {
        {"CODE", kCode, 0x10000,  std::nullopt,  true },
        {"RAM",  kRam,  kRamSize, uint8_t{0xAA}, false},
    };
    return p;
}

ModuleSpec flash_spec() {
    return {
        "flash", "FLASH", {{"xip", kXip}, {"ctrl", kFlashCtrl}},
          {{"jedec_id", 0xEF4016}}
    };
}
// The watchdog clock is gated off by bit 8 of ICU word 0x08 until software clears it.
ModuleSpec icu_spec() {
    return {
        "intc",
        "ICU",
        {{"regs", kIcu}},
        {{"reset_words", ParamTable{{"0x08", 0x100}}},
          {"signals", ParamTable{{"wdt_off", ParamTable{{"offset", 8}, {"shift", 8}}}}}}
    };
}
ModuleSpec wdt_spec() { return {"wdt", "WDT", {{"regs", kWdt}}, {{"clock_off_signal", "ICU.wdt_off"}}}; }
// 1 MHz timer clock: a counter of N is N microseconds.
ModuleSpec timer_spec() {
    return {
        "timer", "TIMER", {{"regs", kTimer}},
          {{"intc", "ICU"}, {"irq", 5}, {"hz", 1000000}}
    };
}
ModuleSpec uart_spec() {
    return {
        "uart", "UART", {{"regs", kUart}},
          {{"intc", "ICU"}, {"irq", 0}}
    };
}

Profile full_chip() {
    Profile p = chip();
    p.modules = {flash_spec(), icu_spec(), wdt_spec(), timer_spec(), uart_spec()};
    return p;
}

// Test-only module type "probe": logs ticks and resets, reports events from a
// callback and, with reset_at, resets the chip from its tick.
struct World {
    std::vector<std::string>                                                log;
    std::map<std::string, std::function<std::optional<uint64_t>(uint64_t)>> next;
    std::map<std::string, std::vector<std::string>>                         param_keys;
    std::map<std::string, bool>                                             has_irq;
    std::map<std::string, std::vector<std::string>>                         signal_names;
};

struct Probe : Module {
    std::string             id;
    World                  &world;
    const Clock            &clock;
    std::function<void()>   reset_chip;
    std::optional<uint64_t> reset_at;

    Probe(const ModuleContext &ctx, World &w) : id(ctx.name), world(w), clock(ctx.clock), reset_chip(ctx.reset_chip) {
        int64_t at = ctx.integer("reset_at", -1);
        if (at >= 0) reset_at = static_cast<uint64_t>(at);
    }
    std::string        name() const override { return id; }
    std::vector<Range> ranges() const override { return {}; }
    uint32_t           read(const std::string &, uint32_t, uint32_t) override { return 0; }
    void               write(const std::string &, uint32_t, uint32_t, uint32_t) override {}
    void               tick(uint64_t now) override {
        world.log.push_back(id + "@" + std::to_string(now));
        if (reset_at && now >= *reset_at) {
            reset_at.reset();
            reset_chip();
        }
    }
    std::optional<uint64_t> next_event_ns() const override {
        auto it = world.next.find(id);
        if (it == world.next.end()) return std::nullopt;
        return it->second(clock.now_ns());
    }
    void reset() override { world.log.push_back("reset " + id); }
};

ModuleRegistry with_probe(World &world) {
    ModuleRegistry r = builtin_registry();
    r["probe"]       = [&world](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        for (const auto &[k, v] : ctx.params.entries) world.param_keys[ctx.name].push_back(k);
        world.has_irq[ctx.name] = static_cast<bool>(ctx.irq);
        for (const auto &[k, s] : ctx.signals) world.signal_names[ctx.name].push_back(k);
        return std::make_unique<Probe>(ctx, world);
    };
    return r;
}

ModuleSpec probe(std::string name, ParamTable params = {}) { return {"probe", std::move(name), {}, std::move(params)}; }

// Code that never waits: b .
void spin(Machine &m) {
    Asm a;
    a.b(0);
    a.load(m);
    m.cpu().reset(0);
}

// Code that only waits: wfi; b wfi
void idle(Machine &m) {
    Asm a;
    a.emit(kWfi);
    a.b(0);
    a.load(m);
    m.cpu().reset(0);
}

} // namespace

TEST_CASE("idle jumps to next event") {
    Machine m(full_chip(), builtin_registry());
    Asm     a;
    a.poke(kTimer + 0x00, 50); // timer 0: 50 us
    a.poke(kTimer + 0x0C, 1);  // enable timer 0
    uint32_t wait = a.pc;
    a.emit(kWfi);
    a.b(wait);
    a.load(m);
    m.cpu().reset(0);

    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 50000); // the event, not one slice (10000)
    CHECK(m.cpu().reg(Reg::PC) == wait + 4);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 100000); // the next period
    REQUIRE(m.module("TIMER"));
    CHECK((m.module("TIMER")->read("regs", 0x0C, 4) >> 7 & 1) == 1); // timer 0 fired
}

TEST_CASE("idle without events advances one slice") {
    Machine m(chip(), builtin_registry());
    idle(m);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 10000);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 20000);
    CHECK(m.cpu().reg(Reg::PC) == 4);
}

TEST_CASE("budget advances 1000 ns per 100 insns") {
    Machine m(chip(), builtin_registry());
    spin(m);
    for (int i = 0; i < 3; ++i) CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 30000);
    CHECK(m.cpu().reg(Reg::PC) == 0);

    Profile slow      = chip();
    slow.insns_per_us = 3; // 1000 * 1000 / 3, rounded down
    Machine s(slow, builtin_registry());
    spin(s);
    CHECK(s.scheduler().step());
    CHECK(s.scheduler().now_ns() == 333333);
    CHECK(s.scheduler().step());
    CHECK(s.scheduler().now_ns() == 666666);
}

TEST_CASE("event due equal to now ignored") {
    World   w;
    Profile p   = chip();
    p.modules   = {probe("P")};
    w.next["P"] = [](uint64_t now) { return std::optional<uint64_t>(now); };
    Machine m(p, with_probe(w));
    idle(m);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 10000);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 20000);
}

TEST_CASE("idle target ignores events not later than now") {
    World   w;
    Profile p       = chip();
    p.modules       = {probe("EARLY"), probe("LATE")};
    w.next["EARLY"] = [](uint64_t) { return std::optional<uint64_t>(5000); };
    w.next["LATE"]  = [](uint64_t) { return std::optional<uint64_t>(25000); };
    Machine m(p, with_probe(w));
    idle(m);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 5000); // smallest event later than 0
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 25000); // 5000 is no longer later than now
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 35000); // nothing later: one slice
    CHECK(w.log == std::vector<std::string>{
                       "EARLY@5000", "LATE@5000", "EARLY@25000", "LATE@25000", "EARLY@35000", "LATE@35000"});
}

TEST_CASE("stopped cpu stops the scheduler") {
    World   w;
    Profile p = chip();
    p.modules = {probe("P")};
    Machine m(p, with_probe(w));
    spin(m);
    CHECK(m.scheduler().step());
    m.cpu().stop();
    CHECK_FALSE(m.scheduler().step());
    CHECK(m.scheduler().stopped());
    CHECK(m.scheduler().now_ns() == 10000);
    CHECK(w.log == std::vector<std::string>{"P@10000"}); // no tick once stopped
}

TEST_CASE("run_forever runs until asked to stop or stopped") {
    Machine m(chip(), builtin_registry());
    spin(m);
    int asked = 0;
    m.scheduler().run_forever([&] { return ++asked > 5; });
    CHECK(asked == 6);
    CHECK(m.scheduler().now_ns() == 50000);

    m.cpu().stop();
    m.scheduler().run_forever([] { return false; });
    CHECK(m.scheduler().stopped());
    CHECK(m.scheduler().now_ns() == 50000);
}

TEST_CASE("scheduler rejects zero insns_per_us") {
    Cpu cpu;
    CHECK_THROWS_AS(Scheduler(cpu, 0), std::invalid_argument);
}

TEST_CASE("machine assembles minimal profile") {
    Sink    sink;
    Machine m(full_chip(),
              builtin_registry(),
              {
                  {"UART", &sink}
    });

    CHECK(m.module("FLASH"));
    CHECK(m.module("ICU"));
    CHECK(m.module("WDT"));
    CHECK(m.module("TIMER"));
    CHECK(m.module("UART"));
    CHECK(m.module("NOPE") == nullptr);
    REQUIRE(m.module("TIMER"));
    CHECK(m.module("TIMER")->name() == "TIMER");

    auto regions = m.bus().memory_regions();
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].name == "CODE");
    CHECK(regions[1].name == "RAM");
    CHECK(regions[1].base == kRam);
    CHECK_FALSE(regions[1].executable);

    std::vector<std::string> claimed;
    for (const auto &r : m.bus().module_ranges())
        claimed.push_back(r.module + "." + r.range + "@" + std::to_string(r.base));
    CHECK(claimed == std::vector<std::string>{
                         "FLASH.xip@" + std::to_string(kXip),
                         "FLASH.ctrl@" + std::to_string(kFlashCtrl),
                         "ICU.regs@" + std::to_string(kIcu),
                         "WDT.regs@" + std::to_string(kWdt),
                         "TIMER.regs@" + std::to_string(kTimer),
                         "UART.regs@" + std::to_string(kUart),
                     });

    Asm a;
    a.ldr_const(4, kFlashCtrl);
    a.emit(ldr(0, 4, 0x10)); // r0 = JEDEC id
    a.ldr_const(4, kIcu);
    a.emit(ldr(1, 4, 0x08)); // r1 = ICU word 0x08 (reset value)
    a.ldr_const(4, kUart);
    a.emit(mov(2, 'H'));
    a.emit(str(2, 4, 0x0C)); // UART data
    a.ldr_const(4, kTimer);
    a.ldr_const(2, 1234);
    a.emit(str(2, 4, 0x00));
    a.emit(ldr(3, 4, 0x00)); // r3 = timer 0 reload, read back
    a.ldr_const(4, kRam);
    a.ldr_const(2, 0xCAFEF00D);
    a.emit(str(2, 4, 0));
    uint32_t done = a.pc;
    a.b(done);
    a.load(m);

    // The machine starts with the CPU reset to address 0.
    CHECK(m.cpu().reg(Reg::PC) == 0);
    CHECK(m.cpu().reg(Reg::CPSR) == 0xD3);
    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 10000);
    CHECK(m.cpu().reg(Reg::PC) == done);
    CHECK(m.cpu().reg(Reg::R0) == 0xEF4016);
    CHECK(m.cpu().reg(Reg::R1) == 0x100);
    CHECK(m.cpu().reg(Reg::R3) == 1234);
    CHECK(word_at(m, kRam) == 0xCAFEF00D);
    CHECK(sink.out.empty()); // UART output is batched for 1 ms of device time
    m.module("UART")->tick(m.scheduler().now_ns() + 1'000'000);
    CHECK(sink.out == std::vector<uint8_t>{'H'});
}

TEST_CASE("unknown module type error text") {
    Profile p = chip();
    p.modules = {
        {"bogus", "BOGUS", {{"regs", 0x00802800}}, {}}
    };
    // The known types are listed in registry order, so the full text changes with every new module type.
    CHECK_THROWS_WITH_AS(Machine(p, builtin_registry()),
                         doctest::Contains("unknown module type 'bogus' (known: flash, "),
                         std::invalid_argument);
}

TEST_CASE("unknown and wifi_mac take a log_level for their stand-in registers") {
    std::ostringstream out;
    Logger::reset_for_tests();
    Logger::configure(out, false);
    Profile    p = chip();
    ModuleSpec blk{"unknown", "BLK", {{"regs", 0x00802D00}}, {}};
    ModuleSpec mac{
        "wifi_mac", "MAC", {{"core", 0xC0000000}, {"pl", 0xC0008000}},
          {}
    };
    blk.params.set("log_level", "debug");
    mac.params.set("log_level", "Debug");
    p.modules = {blk, mac};
    Machine m(p, builtin_registry());
    m.module("BLK")->write("regs", 0x00, 4, 1);
    m.module("MAC")->write("pl", 0x100, 4, 2);
    CHECK(out.str().empty());
    Logger::set_level("unknown", Level::Debug);
    m.module("BLK")->write("regs", 0x00, 4, 1);
    m.module("MAC")->write("pl", 0x100, 4, 2);
    CHECK(out.str().find("DEBUG emulator.unknown: BLK write +0x0000") != std::string::npos);
    CHECK(out.str().find("DEBUG emulator.unknown: MAC_PL write +0x0100") != std::string::npos);
    Logger::reset_for_tests();

    blk.params.set("log_level", "loud");
    p.modules = {blk};
    CHECK_THROWS_WITH_AS(Machine(p, builtin_registry()),
                         "module BLK: log_level must be debug, info, warning or error",
                         std::invalid_argument);
}

TEST_CASE("intc must precede user") {
    Profile p = chip();
    p.modules = {timer_spec(), icu_spec()};
    CHECK_THROWS_WITH_AS(Machine(p, builtin_registry()),
                         "module TIMER needs ICU listed before it in the profile",
                         std::invalid_argument);

    p.modules = {timer_spec()}; // not listed at all
    CHECK_THROWS_WITH_AS(Machine(p, builtin_registry()),
                         "module TIMER needs ICU listed before it in the profile",
                         std::invalid_argument);
}

TEST_CASE("signal owner declared after user rejected") {
    Profile p = chip();
    p.modules = {wdt_spec(), icu_spec()};
    CHECK_THROWS_WITH_AS(
        Machine(p, builtin_registry()), "module WDT needs ICU listed before it in the profile", std::invalid_argument);
}

TEST_CASE("bases missing a range name rejected") {
    Profile    p = chip();
    ModuleSpec f = flash_spec();
    f.bases.erase("ctrl");
    p.modules = {f};
    CHECK_THROWS_WITH_AS(Machine(p, builtin_registry()), "FLASH: no base for range ctrl", std::invalid_argument);
}

TEST_CASE("unknown profile params ignored") {
    World      w;
    Profile    p = chip();
    ModuleSpec t = timer_spec();
    t.params.set("bogus", 7);
    t.params.set("colour", "blue");
    p.modules = {
        icu_spec(),
        t,
        probe("P",
              {{"bogus", 7},
                    {"intc", "ICU"},
                    {"irq", 3},
                    {"x_signal", "ICU.wdt_off"},
                    {"nested", ParamTable{{"a", 1}}}}
              )
    };
    Machine m(p, with_probe(w));
    REQUIRE(m.module("TIMER"));
    // Resolved collaborators are taken out; the rest reaches the factory as given.
    CHECK(w.param_keys["P"] == std::vector<std::string>{"bogus", "nested"});
    CHECK(w.has_irq["P"]);
    CHECK(w.signal_names["P"] == std::vector<std::string>{"x"});
}

TEST_CASE("irq table becomes named lines") {
    World   w;
    Profile p = chip();
    p.modules = {
        icu_spec(), probe("P", {{"intc", "ICU"}, {"irq", ParamTable{{"rx", 1}, {"tx", 2}}}}
          )
    };
    ModuleRegistry           r = with_probe(w);
    std::vector<std::string> names;
    r["probe"] = [&](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        for (const auto &[k, l] : ctx.irqs) {
            names.push_back(k);
            l(true); // raise the source
        }
        return std::make_unique<Probe>(ctx, w);
    };
    Machine m(p, r);
    CHECK(names == std::vector<std::string>{"rx", "tx"});
    REQUIRE(m.module("ICU"));
    CHECK(m.module("ICU")->read("regs", 0x48, 4) == 0b110); // raw sources 1 and 2
}

TEST_CASE("chip reset refills ram, keeps flash, resets cpu") {
    Sink    sink;
    Machine m(full_chip(),
              builtin_registry(),
              {
                  {"UART", &sink}
    });
    spin(m);
    for (int i = 0; i < 3; ++i) CHECK(m.scheduler().step());

    auto *flash = dynamic_cast<Flash *>(m.module("FLASH"));
    REQUIRE(flash);
    std::vector<uint8_t> block(34);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<uint8_t>(i * 7);
    flash->load(0, block);
    auto flash_before = flash->raw();

    const uint8_t marker[4] = {1, 2, 3, 4};
    m.bus().write_memory(kRam + 0x100, marker);
    m.bus().write_memory(0x400, marker); // CODE has no reset fill
    REQUIRE(m.module("TIMER"));
    m.module("TIMER")->write("regs", 0x00, 4, 77);
    m.cpu().set_reg(Reg::R0, 5);
    m.cpu().set_reg(Reg::PC, 0x100);

    m.reset();

    auto ram = m.cpu().mem_read(kRam, kRamSize);
    CHECK(ram == std::vector<uint8_t>(kRamSize, 0xAA));
    CHECK(m.cpu().mem_read(0x400, 4) == std::vector<uint8_t>{1, 2, 3, 4});
    CHECK(flash->raw() == flash_before);
    CHECK(m.module("TIMER")->read("regs", 0x00, 4) == 0);
    CHECK(m.module("ICU")->read("regs", 0x08, 4) == 0x100);
    CHECK(m.cpu().reg(Reg::PC) == 0);
    CHECK(m.cpu().reg(Reg::R0) == 0);
    CHECK(m.cpu().reg(Reg::CPSR) == 0xD3);
    CHECK(m.scheduler().now_ns() == 30000);        // device time is not reset
    m.module("UART")->write("regs", 0x0C, 4, 'k'); // endpoint still attached
    m.module("UART")->tick(m.scheduler().now_ns() + 1'000'000);
    CHECK(sink.out == std::vector<uint8_t>{'k'});
}

TEST_CASE("reset during tick is deterministic") {
    World   w;
    Profile p = chip();
    p.modules = {
        probe("A"), probe("B", {{"reset_at", 20000}}
          ), probe("C")
    };
    Machine m(p, with_probe(w));
    spin(m);
    for (int i = 0; i < 3; ++i) CHECK(m.scheduler().step());
    // B resets the chip from its tick: every module is reset once, then the
    // rest of that step's ticks run as usual.
    CHECK(w.log == std::vector<std::string>{"A@10000",
                                            "B@10000",
                                            "C@10000",
                                            "A@20000",
                                            "B@20000",
                                            "reset A",
                                            "reset B",
                                            "reset C",
                                            "C@20000",
                                            "A@30000",
                                            "B@30000",
                                            "C@30000"});
}

TEST_CASE("watchdog expiry resets machine end to end") {
    std::ostringstream out;
    Logger::reset_for_tests();
    Logger::configure(out, false);
    struct Restore {
        ~Restore() { Logger::reset_for_tests(); }
    } restore;

    Machine m(full_chip(), builtin_registry());
    Asm     a;
    a.poke(kIcu + 0x08, 0);   // ungate the watchdog clock
    a.poke(kRam, 0x12345678); // RAM marker
    a.poke(kWdt, 0x5A0001);   // period 1 ms, first key
    a.poke(kWdt, 0xA50001);   // second key: kick
    uint32_t loop = a.pc;
    a.b(loop);
    a.load(m);

    // 1 ms is 100 slices of 10 us; the watchdog fires on the 100th tick.
    for (int i = 0; i < 99; ++i) REQUIRE(m.scheduler().step());
    CHECK(m.cpu().reg(Reg::PC) == loop);
    CHECK(word_at(m, kRam) == 0x12345678);
    CHECK(m.scheduler().now_ns() == 990000);

    CHECK(m.scheduler().step());
    CHECK(m.scheduler().now_ns() == 1000000);
    CHECK(m.cpu().reg(Reg::PC) == 0);
    CHECK(m.cpu().reg(Reg::CPSR) == 0xD3);
    CHECK(word_at(m, kRam) == 0xAAAAAAAA);
    CHECK(word_at(m, loop) == 0xEAFFFFFE); // code survives (b .)
    CHECK(out.str() == "WARNING emulator.wdt: watchdog expired: resetting the chip\n");
    REQUIRE(m.module("WDT"));
    CHECK(m.module("WDT")->read("regs", 0, 4) == 0x4000);
    CHECK(m.module("ICU")->read("regs", 0x08, 4) == 0x100);

    // The program runs again from the start.
    CHECK(m.scheduler().step());
    CHECK(m.cpu().reg(Reg::PC) == loop);
    CHECK(word_at(m, kRam) == 0x12345678);
}

TEST_CASE("irq without intc rejected") {
    World   w;
    Profile p = chip();
    p.modules = {
        icu_spec(), probe("P", {{"irq", 3}}
          )
    };
    CHECK_THROWS_WITH_AS(Machine(p, with_probe(w)), "module P has irq but no intc", std::invalid_argument);
    ModuleSpec t = timer_spec();
    t.params.erase("intc");
    p.modules = {icu_spec(), t};
    CHECK_THROWS_WITH_AS(Machine(p, builtin_registry()), "module TIMER has irq but no intc", std::invalid_argument);
}

namespace {
// Builds ICU with `signals` and a probe whose x_signal is ICU.s; returns the
// probe's signal.
Signal icu_signal(const ParamValue &spec, uint32_t word8, std::unique_ptr<Machine> &keep) {
    World   w;
    Profile p = chip();
    p.modules = {
        {"intc",
         "ICU", {{"regs", kIcu}},
         {{"reset_words", ParamTable{{"0x08", static_cast<int64_t>(word8)}}}, {"signals", ParamTable{{"s", spec}}}}},
        probe("P", {{"x_signal", "ICU.s"}}
        )
    };
    Signal         got;
    ModuleRegistry r = with_probe(w);
    r["probe"]       = [&](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        got = ctx.signal("x");
        return std::make_unique<Probe>(ctx, w);
    };
    keep = std::make_unique<Machine>(p, r);
    return got;
}
} // namespace

TEST_CASE("intc signal list form is offset and bit") {
    std::unique_ptr<Machine> m;
    Signal                   s = icu_signal(ParamList{0x08, 8}, 0x100, m);
    REQUIRE(s);
    CHECK(s());
    m->module("ICU")->write("regs", 0x08, 4, 0xFFFFFEFF);
    CHECK_FALSE(s());
    m->module("ICU")->write("regs", 0x08, 4, 0x100);
    CHECK(s());

    Signal top = icu_signal(ParamList{0x08, 31}, 0x80000000u, m);
    CHECK(top());
}

TEST_CASE("intc signal table form still accepted") {
    std::unique_ptr<Machine> m;
    Signal                   s = icu_signal(
        ParamTable{
                              {"offset", 8},
                              {"shift",  4},
                              {"width",  4},
                              {"equals", 5}
    },
        0x50,
        m);
    REQUIRE(s);
    CHECK(s());
    m->module("ICU")->write("regs", 0x08, 4, 0x60);
    CHECK_FALSE(s());
}

TEST_CASE("intc signal bad list form rejected") {
    std::unique_ptr<Machine> m;
    for (const ParamList &bad : {
             ParamList{8},
             ParamList{8, 8, 1},
             ParamList{8, 32},
             ParamList{-1, 0}
    }) {
        CHECK_THROWS_AS(icu_signal(bad, 0, m), std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(
        icu_signal(ParamList{8}, 0, m), "module ICU: signal s must be [offset, bit] or a table", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        icu_signal("bogus", 0, m), "module ICU: signal s must be [offset, bit] or a table", std::invalid_argument);
}

namespace {

constexpr uint32_t kGdma = 0x00809000;

ModuleSpec gdma_spec() {
    return {
        "gdma", "GDMA", {{"regs", kGdma}},
          {{"intc", "ICU"}, {"irq", 15}}
    };
}

// Builds ICU, GDMA and a probe with `probe_params`; keeps what the probe was given.
struct DmaWorld {
    World                        world;
    std::shared_ptr<RequestLine> line;
    BusMemory                   *memory = nullptr;
    std::unique_ptr<Machine>     machine;

    explicit DmaWorld(ParamTable probe_params, bool gdma_first = true) {
        Profile p = chip();
        p.modules = {icu_spec()};
        if (gdma_first) p.modules.push_back(gdma_spec());
        p.modules.push_back(probe("P", std::move(probe_params)));
        if (!gdma_first) p.modules.push_back(gdma_spec());
        ModuleRegistry r = with_probe(world);
        r["probe"]       = [this](const ModuleContext &ctx) -> std::unique_ptr<Module> {
            line   = ctx.dma;
            memory = ctx.memory;
            return std::make_unique<Probe>(ctx, world);
        };
        machine = std::make_unique<Machine>(p, r);
    }
};

} // namespace

TEST_CASE("dma and dma_request give the module a request line of that engine") {
    DmaWorld w({
        {"dma",         "GDMA"},
        {"dma_request", 8     }
    });
    REQUIRE(w.line);
    CHECK(w.memory != nullptr);
    // The parameters are consumed.
    CHECK(w.world.param_keys["P"].empty());
    // The line is the one the engine uses: a channel whose source is request 8 is paced by it.
    Module *dma = w.machine->module("GDMA");
    w.line->bind([] { return std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0x44}; });
    w.line->set(true);
    dma->write("regs", 0x04, 4, kRam + 0x100);                                   // destination
    dma->write("regs", 0x1C, 4, 8);                                              // source request 8
    dma->write("regs", 0x00, 4, (3u << 16) | (2u << 6) | (2u << 4) | 0x200 | 1); // 4 bytes, 32-bit, dst +4
    dma->tick(w.machine->scheduler().now_ns() + 1'000'000);
    CHECK(word_at(*w.machine, kRam + 0x100) == 0x44332211);
}

TEST_CASE("dma request line errors") {
    CHECK_THROWS_WITH_AS(DmaWorld({
                             {"dma", "GDMA"}
    }),
                         "module P: dma needs dma_request 0-15",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(DmaWorld({
                             {"dma",         "GDMA"},
                             {"dma_request", 16    }
    }),
                         "module P: dma needs dma_request 0-15",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(DmaWorld({
                             {"dma",         3},
                             {"dma_request", 8}
    }),
                         "module P: dma must be a module name",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(DmaWorld(
                             {
                                 {"dma",         "GDMA"},
                                 {"dma_request", 8     }
    },
                             false),
                         "module P needs GDMA listed before it in the profile",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(DmaWorld({
                             {"dma",         "ICU"},
                             {"dma_request", 8    }
    }),
                         "ICU: not a DMA engine (asked for request line 8)",
                         std::invalid_argument);
}

TEST_CASE("the jpeg module's frame reaches RAM through the GDMA request line") {
    Profile p = chip();
    p.modules = {
        icu_spec(),
        gdma_spec(),
        {"jpeg",
            "JPEG", {{"regs", 0x0080A000}},
            {{"intc", "ICU"},
          {"irq", 29},
          {"dma", "GDMA"},
          {"dma_request", 8},
          {"frame_interval_ms", 1},
          {"encode_ms", 1}}}
    };
    Machine m(p, builtin_registry());
    Module *jpeg = m.module("JPEG");
    Module *dma  = m.module("GDMA");

    // A DMA channel from the JPEG request line into RAM, 64 bytes, destination +4 per word.
    dma->write("regs", 0x04, 4, kRam + 0x400);
    dma->write("regs", 0x1C, 4, 8);
    dma->write("regs", 0x00, 4, (63u << 16) | (2u << 6) | (2u << 4) | 0x200 | 2 | 1);
    // The encoder: byte order natural, frame after 1 ms.
    jpeg->write("regs", 0x04, 4, (1u << 1) | (1u << 4));
    jpeg->tick(1'000'000); // start_frame: the request line goes up
    dma->tick(2'000'000);  // the DMA moves the words
    CHECK((dma->read("regs", 0x00, 4) & 1) == 0);
    CHECK(dma->read("regs", 0xE0, 4) == 1);
    auto bytes = m.cpu().mem_read(kRam + 0x400, 64);
    CHECK(bytes[0] == 0xFF);
    CHECK(bytes[1] == 0xD8);
    CHECK(bytes[2] != 0xAA); // RAM was filled with 0xAA at reset: these bytes were written
    CHECK(bytes[63] != 0xAA);
    // 16 words went out of the stream; the rest is still there.
    CHECK((jpeg->read("regs", 0x20, 4) & 1) == 0);
}

TEST_CASE("a memory to memory transfer goes through the real bus") {
    DmaWorld      w({});
    Machine      &m     = *w.machine;
    const uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
    m.bus().write_memory(kRam + 0x200, src);
    Module *dma = m.module("GDMA");
    dma->write("regs", 0x04, 4, kRam + 0x300);
    dma->write("regs", 0x08, 4, kRam + 0x200);
    dma->write("regs", 0x00, 4, (7u << 16) | (2u << 6) | (2u << 4) | 0x300 | 2 | 1); // 8 bytes, +4/+4, fin int
    dma->tick(m.scheduler().now_ns() + 1'000'000);
    CHECK(word_at(m, kRam + 0x300) == 0x04030201);
    CHECK(word_at(m, kRam + 0x304) == 0x08070605);
    CHECK(dma->read("regs", 0xE0, 4) == 1);

    // A transfer that leaves RAM aborts the channel and says so.
    std::ostringstream out;
    Logger::reset_for_tests();
    Logger::configure(out, false);
    dma->write("regs", 0x04, 4, kRam + kRamSize - 4);
    dma->write("regs", 0x00, 4, (7u << 16) | (2u << 6) | (2u << 4) | 0x300 | 1);
    dma->tick(m.scheduler().now_ns() + 2'000'000);
    CHECK((dma->read("regs", 0x00, 4) & 1) == 0);
    CHECK(out.str().find("ch0 aborted: bus error at 0x00410000: nothing writable there") != std::string::npos);
    Logger::reset_for_tests();
}
