#include "app/registry.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "periph/flash.h"
#include "periph/gdma.h"
#include "periph/gpio.h"
#include "periph/i2c1.h"
#include "periph/default_picture.h"
#include "periph/intc.h"
#include "periph/jpeg.h"
#include "periph/saradc.h"
#include "periph/sctrl.h"
#include "periph/sdhost.h"
#include "periph/timer.h"
#include "periph/trng.h"
#include "periph/uart.h"
#include "periph/unknown.h"
#include "periph/wifi/mac.h"
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
std::map<uint32_t, uint32_t> reset_words(const ModuleContext &ctx, const std::string &param = "reset_words") {
    std::map<uint32_t, uint32_t> words;
    const ParamTable            *t = table_param(ctx, param);
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
            bad(ctx, param + " key '" + key + "' is not an offset");
        if (!value.integer()) bad(ctx, param + " value for " + key + " must be an integer");
        words[static_cast<uint32_t>(offset)] = u32(ctx, param + " " + key, *value.integer());
    }
    return words;
}

// A list of { offset, shift, width, add } tables.
std::vector<ReadAdjust> read_adjust(const ModuleContext &ctx) {
    std::vector<ReadAdjust> out;
    const ParamValue       *v = ctx.params.find("read_adjust");
    if (!v) return out;
    const ParamList *empty = v->list();
    if (empty && empty->empty()) return out;
    if (!v->tables()) bad(ctx, "read_adjust must be a list of tables");
    for (const ParamTable &t : *v->tables()) {
        auto field = [&](const char *key) -> int64_t {
            const ParamValue *f = t.find(key);
            if (!f || !f->integer()) bad(ctx, std::string("read_adjust needs an integer ") + key);
            return *f->integer();
        };
        int64_t shift = field("shift"), width = field("width");
        if (shift < 0 || shift > 31 || width < 1 || width > 32 || shift + width > 32)
            bad(ctx, "read_adjust field does not fit in 32 bits");
        out.push_back({u32(ctx, "read_adjust offset", field("offset")),
                       static_cast<unsigned>(shift),
                       static_cast<unsigned>(width),
                       u32(ctx, "read_adjust add", field("add"))});
    }
    return out;
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

// The level of a stand-in's access log lines: "debug", "info", "warning" or
// "error" (any letter case), INFO when not given.
Level log_level(const ModuleContext &ctx) {
    const ParamValue *v = ctx.params.find("log_level");
    if (!v) return Level::Info;
    std::optional<Level> level = v->string() ? Logger::parse_level(*v->string()) : std::nullopt;
    if (!level) bad(ctx, "log_level must be none, debug, info, warning or error");
    return *level;
}

} // namespace

ModuleRegistry builtin_registry() {
    ModuleRegistry r;
    r["flash"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        Flash::Options options;
        options.config_reset  = u32_param(ctx, "config_reset", Flash::kCrcEn);
        options.m_value_reset = u32_param(ctx, "m_value_reset", 0);
        if (options.m_value_reset > 0xFF) bad(ctx, "m_value_reset must be a byte");
        options.read_ptr_reset = u32_param(ctx, "read_ptr_reset", 0);
        if (options.read_ptr_reset > 7) bad(ctx, "read_ptr_reset must be 0-7");
        options.words = reset_words(ctx);
        return std::make_unique<Flash>(
            ctx.name, u32_param(ctx, "size", 0x200000), u32_param(ctx, "jedec_id", 0x1C7015), std::move(options));
    };
    r["gdma"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        uint32_t bus_hz = u32_param(ctx, "bus_hz", 120000000);
        if (bus_hz == 0) bad(ctx, "bus_hz must not be 0");
        if (!ctx.memory) bad(ctx, "needs a bus master port");
        return std::make_unique<Gdma>(ctx.name, ctx.clock, ctx.irq, *ctx.memory, bus_hz);
    };
    r["gpio"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Gpio>(ctx.name, ctx.irq, u32_param(ctx, "pin_reset", 0), reset_words(ctx));
    };
    r["i2c1"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        uint32_t ref_hz = u32_param(ctx, "ref_hz", 26000000);
        if (ref_hz == 0) bad(ctx, "ref_hz must not be 0");
        return std::make_unique<I2c1>(ctx.name, ctx.clock, ctx.irq, ref_hz);
    };
    r["intc"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Intc>(ctx.name, ctx.cpu_irq, ctx.cpu_fiq, reset_words(ctx), signal_specs(ctx));
    };
    r["jpeg"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        std::vector<uint8_t> picture(kDefaultPicture, kDefaultPicture + kDefaultPicture_size);
        if (const ParamValue *path = ctx.params.find("picture")) {
            if (!path->string()) bad(ctx, "picture must be a file name");
            std::ifstream file(*path->string(), std::ios::binary);
            if (!file) bad(ctx, "cannot read picture '" + *path->string() + "'");
            picture.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        return std::make_unique<Jpeg>(ctx.name,
                                      ctx.clock,
                                      ctx.irq,
                                      ctx.dma,
                                      u32_param(ctx, "frame_interval_ms", 1000),
                                      u32_param(ctx, "encode_ms", 150),
                                      std::move(picture));
    };
    r["saradc"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Saradc>(ctx.name, ctx.irq, u32_param(ctx, "sample", 330));
    };
    r["sdhost"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        uint32_t size = u32_param(ctx, "size", 0x100);
        if (size <= SdHost::kStatus) bad(ctx, "size must cover the status register");
        return std::make_unique<SdHost>(ctx.name, size, reset_words(ctx), ctx.cpu_instruction, log_level(ctx));
    };
    r["sctrl"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Sctrl>(ctx.name,
                                       u32_param(ctx, "chip_id", 0),
                                       u32_param(ctx, "device_id", 0),
                                       reset_words(ctx),
                                       reset_words(ctx, "warm_reset_words"),
                                       signal_specs(ctx),
                                       read_adjust(ctx));
    };
    r["timer"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Timer>(ctx.name, ctx.clock, ctx.irq, u32_param(ctx, "hz", 26000000));
    };
    r["trng"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> { return std::make_unique<Trng>(ctx.name); };
    r["uart"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        auto uart = std::make_unique<Uart>(ctx.name, ctx.clock, ctx.irq);
        uart->attach(ctx.endpoint);
        return uart;
    };
    r["unknown"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        uint32_t size = u32_param(ctx, "size", 0x100);
        if (size == 0) bad(ctx, "size must not be 0");
        return std::make_unique<Unknown>(ctx.name, size, reset_words(ctx), ctx.cpu_instruction, log_level(ctx));
    };
    r["wifi_mac"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        auto line = [&ctx](const char *key) {
            auto it = ctx.irqs.find(key);
            return it == ctx.irqs.end() ? Line() : it->second;
        };
        WifiMac::Lines lines{line("tx_rx_timer"),
                             line("tx_rx_misc"),
                             line("rx_trigger"),
                             line("tx_trigger"),
                             line("prot_trigger"),
                             line("general")};
        if (!ctx.memory) bad(ctx, "needs a bus master port");
        return std::make_unique<WifiMac>(ctx.name,
                                         ctx.clock,
                                         ctx.signal("mac_powered"),
                                         std::move(lines),
                                         ctx.cpu_instruction,
                                         *ctx.memory,
                                         log_level(ctx));
    };
    r["wdt"] = [](const ModuleContext &ctx) -> std::unique_ptr<Module> {
        return std::make_unique<Wdt>(ctx.name, ctx.clock, ctx.reset_chip, ctx.signal("clock_off"));
    };
    return r;
}

} // namespace bk
