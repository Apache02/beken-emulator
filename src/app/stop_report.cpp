#include "app/stop_report.h"

#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bk {

namespace {

using std::string;

constexpr uint32_t kBitI = 0x80, kBitF = 0x40, kBitT = 0x20;
// The interrupt controller's enable, global and raw registers.
constexpr uint32_t kIntcEnable = 0x40, kIntcGlobal = 0x44, kIntcRaw = 0x48;
constexpr unsigned kStackWords = 8;

string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

string mode_name(uint32_t cpsr) {
    switch (cpsr & 0x1F) {
        case 0x10: return "USR";
        case 0x11: return "FIQ";
        case 0x12: return "IRQ";
        case 0x13: return "SVC";
        case 0x17: return "ABT";
        case 0x1B: return "UND";
        case 0x1F: return "SYS";
        default: return "mode " + hex8(cpsr & 0x1F);
    }
}

// The modules wired to each source of the interrupt controller `intc`, from
// the `intc` and `irq` parameters of the profile: the module name, or
// `MODULE.LINE` for a line of an `irq` table.
std::map<unsigned, string> source_names(const Profile &profile, const string &intc) {
    std::map<unsigned, string> names;
    auto                       add = [&names](int64_t bit, const string &name) {
        if (bit < 0 || bit > 31) return;
        string &s = names[static_cast<unsigned>(bit)];
        s += (s.empty() ? "" : "/") + name;
    };
    for (const ModuleSpec &spec : profile.modules) {
        const ParamValue *to  = spec.params.find("intc");
        const ParamValue *irq = spec.params.find("irq");
        if (!to || !irq || !to->string() || *to->string() != intc) continue;
        if (const int64_t *bit = irq->integer()) add(*bit, spec.name);
        if (const ParamTable *lines = irq->table())
            for (const auto &[line, bit] : lines->entries)
                if (const int64_t *b = bit.integer()) add(*b, spec.name + "." + line);
    }
    return names;
}

// The source numbers of the set bits with the modules wired to them, for
// example "0 UART1, 8 TIMER0/TIMER1, 11", or "none".
string sources(uint32_t bits, const std::map<unsigned, string> &names) {
    string out;
    for (unsigned b = 0; b < 32; ++b) {
        if (!(bits >> b & 1)) continue;
        out += (out.empty() ? "" : ", ") + std::to_string(b);
        if (auto it = names.find(b); it != names.end()) out += " " + it->second;
    }
    return out.empty() ? "none" : out;
}

} // namespace

std::string stop_report(const Profile &profile, Machine &machine) {
    Cpu           &cpu  = machine.cpu();
    const uint32_t cpsr = cpu.reg(Reg::CPSR);
    const uint64_t ns   = machine.scheduler().now_ns();
    char           line[160];
    string         out = "\n";

    std::snprintf(line,
                  sizeof line,
                  "machine state at stop, device time %llu.%06llu s\n",
                  static_cast<unsigned long long>(ns / 1'000'000'000),
                  static_cast<unsigned long long>(ns / 1000 % 1'000'000));
    out += line;

    out += "cpu\n";
    out += "  pc    " + hex8(cpu.reg(Reg::PC));
    if (std::optional<Instruction> insn = cpu.instruction()) {
        std::snprintf(line, sizeof line, "  instruction %0*X", static_cast<int>(insn->width * 2), insn->opcode);
        out += line;
    }
    out += "\n";
    out += "  cpsr  " + hex8(cpsr) + "  " + mode_name(cpsr) + ((cpsr & kBitT) ? ", Thumb" : ", ARM") +
           ((cpsr & kBitI) ? ", IRQ masked" : "") + ((cpsr & kBitF) ? ", FIQ masked" : "") + "\n";
    if ((cpsr & 0x1F) != 0x10 && (cpsr & 0x1F) != 0x1F) out += "  spsr  " + hex8(cpu.reg(Reg::SPSR)) + "\n";
    out += string("  lines IRQ ") + (cpu.irq_raised() ? "raised" : "low") + ", FIQ " +
           (cpu.fiq_raised() ? "raised" : "low") + "\n";
    static const char *const kNames[] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr"};
    for (int r = 0; r < 15; ++r) {
        std::snprintf(line, sizeof line, "  %-4s %s", kNames[r], hex8(cpu.reg(static_cast<Reg>(r))).c_str());
        out += line;
        if (r % 4 == 3 || r == 14) out += "\n";
    }

    const uint32_t sp = cpu.reg(Reg::SP);
    out += "stack at sp " + hex8(sp) + "\n";
    try {
        const std::vector<uint8_t> bytes = cpu.mem_read(sp, kStackWords * 4);
        for (unsigned i = 0; i < kStackWords; ++i) {
            const uint32_t w = bytes[i * 4] | bytes[i * 4 + 1] << 8 | bytes[i * 4 + 2] << 16 |
                               static_cast<uint32_t>(bytes[i * 4 + 3]) << 24;
            out += (i % 4 == 0 ? "  " : " ") + hex8(w);
            if (i % 4 == 3) out += "\n";
        }
    } catch (const std::exception &) {
        out += "  (not readable)\n";
    }

    for (const ModuleSpec &spec : profile.modules) {
        if (spec.type != "intc") continue;
        Module *m = machine.module(spec.name);
        if (!m) continue;
        const uint32_t enable = m->read("regs", kIntcEnable, 4);
        const uint32_t global = m->read("regs", kIntcGlobal, 4);
        const uint32_t raw    = m->read("regs", kIntcRaw, 4);
        out += "interrupt controller " + spec.name + "\n";
        out +=
            string("  global IRQ ") + ((global & 1) ? "on" : "off") + ", FIQ " + ((global & 2) ? "on" : "off") + "\n";
        const auto names = source_names(profile, spec.name);
        out += "  enabled " + sources(enable, names) + "\n";
        out += "  raised  " + sources(raw, names) + "\n";
        out += "  pending " + sources(raw & enable, names) + "\n";
    }
    return out;
}

} // namespace bk
