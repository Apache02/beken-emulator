#include "app/registry.h"

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "periph/flash.h"
#include "periph/intc.h"
#include "periph/timer.h"
#include "periph/uart.h"
#include "periph/wdt.h"

namespace bk {

namespace {

[[noreturn]] void bad(const ModuleContext &ctx, const std::string &what) {
    throw std::invalid_argument("module " + ctx.name + ": " + what);
}

uint32_t u32(const ModuleContext &ctx, const std::string &key, int64_t value) {
    if (value < 0 || value > 0xFFFFFFFFLL) bad(ctx, key + " out of range");
    return static_cast<uint32_t>(value);
}

uint32_t u32_param(const ModuleContext &ctx, const std::string &key, uint32_t fallback) {
    return u32(ctx, key, ctx.integer(key, fallback));
}

const ParamTable *table_param(const ModuleContext &ctx, const std::string &key) {
    const ParamValue *v = ctx.params.find(key);
    if (!v) return nullptr;
    if (!v->table()) bad(ctx, key + " must be a table");
    return v->table();
}

// Keys are offsets written as strings ("0x08" or "8").
std::map<uint32_t, uint32_t> reset_words(const ModuleContext &ctx) {
    std::map<uint32_t, uint32_t> words;
    const ParamTable            *t = table_param(ctx, "reset_words");
    if (!t) return words;
    for (const auto &[key, value] : t->entries) {
        size_t             used   = 0;
        unsigned long long offset = 0;
        try {
            offset = std::stoull(key, &used, 0);
        } catch (const std::exception &) {
            used = 0;
        }
        if (used == 0 || used != key.size() || offset > 0xFFFFFFFFULL)
            bad(ctx, "reset_words key '" + key + "' is not an offset");
        if (!value.integer()) bad(ctx, "reset_words value for " + key + " must be an integer");
        words[static_cast<uint32_t>(offset)] = u32(ctx, "reset_words " + key, *value.integer());
    }
    return words;
}

// Each signal is either a list [offset, bit] (active while that bit is set) or
// a table { offset, shift = 0, width = 1, equals = 1 }.
std::map<std::string, SignalSpec> signal_specs(const ModuleContext &ctx) {
    std::map<std::string, SignalSpec> specs;
    const ParamTable                 *t = table_param(ctx, "signals");
    if (!t) return specs;
    for (const auto &[name, value] : t->entries) {
        const std::string form = "signal " + name + " must be [offset, bit] or a table";
        if (const ParamList *list = value.list()) {
            if (list->size() != 2) bad(ctx, form);
            int64_t bit = (*list)[1];
            if (bit < 0 || bit > 31) bad(ctx, "signal " + name + ": bit must be 0-31");
            SignalSpec s;
            s.offset    = u32(ctx, "signal offset", (*list)[0]);
            s.shift     = static_cast<unsigned>(bit);
            specs[name] = s;
            continue;
        }
        const ParamTable *fields = value.table();
        if (!fields) bad(ctx, form);
        auto field = [&](const char *key, std::optional<int64_t> fallback) -> int64_t {
            const ParamValue *f = fields->find(key);
            if (!f) {
                if (!fallback) bad(ctx, "signal " + name + " needs " + key);
                return *fallback;
            }
            if (!f->integer()) bad(ctx, "signal " + name + ": " + key + " must be an integer");
            return *f->integer();
        };
        SignalSpec s;
        s.offset      = u32(ctx, "signal offset", field("offset", std::nullopt));
        int64_t shift = field("shift", 0), width = field("width", 1);
        if (shift < 0 || shift > 31 || width < 1 || width > 32 || shift + width > 32)
            bad(ctx, "signal " + name + " field does not fit in 32 bits");
        s.shift  = static_cast<unsigned>(shift);
        s.width  = static_cast<unsigned>(width);
        s.equals = u32(ctx, "signal equals", field("equals", 1));
        for (const auto &[key, unused] : fields->entries) {
            if (key != "offset" && key != "shift" && key != "width" && key != "equals")
                bad(ctx, "signal " + name + " has unknown field " + key);
        }
        specs[name] = s;
    }
    return specs;
}

} // namespace

ModuleRegistry builtin_registry() {
    ModuleRegistry r;
    r["flash"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Flash>(
            ctx.name, u32_param(ctx, "size", 0x200000), u32_param(ctx, "jedec_id", 0x1C7015));
    };
    r["intc"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Intc>(ctx.name, ctx.cpu_irq, ctx.cpu_fiq, reset_words(ctx), signal_specs(ctx));
    };
    r["timer"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Timer>(ctx.name, ctx.clock, ctx.irq, u32_param(ctx, "hz", 26000000));
    };
    r["uart"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        auto uart = std::make_unique<Uart>(ctx.name, ctx.clock, ctx.irq);
        uart->attach(ctx.endpoint);
        return uart;
    };
    r["wdt"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Wdt>(ctx.name, ctx.clock, ctx.reset_chip, ctx.signal("clock_off"));
    };
    return r;
}

} // namespace bk
