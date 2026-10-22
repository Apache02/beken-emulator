#include "core/loader.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

namespace bk {

namespace {

std::string hex32(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%08llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The most any load could accept: the largest RAM region or flash-alias module.
uint64_t max_capacity(Machine &machine) {
    uint64_t cap = 0;
    for (const MemRegion &r : machine.bus().memory_regions()) cap = std::max<uint64_t>(cap, r.size);
    for (const auto &[name, spec] : machine.aliases()) {
        const auto *fa     = std::get_if<FlashAlias>(&spec.target);
        Module     *module = fa ? machine.module(fa->module) : nullptr;
        if (!module || !module->loadable()) continue;
        // Largest size check_load accepts at offset 0 (monotonic in the size).
        uint64_t lo = 0, hi = UINT32_MAX;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo + 1) / 2;
            try {
                module->check_load(0, static_cast<uint32_t>(mid));
                lo = mid;
            } catch (const std::invalid_argument &) {
                hi = mid - 1;
            }
        }
        cap = std::max(cap, lo);
    }
    return cap;
}

// Reads at most `cap` + 1 bytes; a longer file is refused without being read to the end.
std::vector<uint8_t> read_file(const std::filesystem::path &file, uint64_t cap) {
    const std::string                                name = file.string();
    std::unique_ptr<std::FILE, int (*)(std::FILE *)> f(std::fopen(name.c_str(), "rb"), std::fclose);
    if (!f) throw LoadError("cannot read " + name + ": " + std::strerror(errno));
    std::vector<uint8_t> data;
    uint8_t              buf[65536];
    size_t               n;
    while ((n = std::fread(buf, 1, sizeof buf, f.get())) > 0) {
        // One byte past the cap is kept so the target's own "does not fit" check reports it.
        if (n > cap + 1 - data.size())
            throw LoadError(name + ": file is too large (more than " + std::to_string(cap) +
                            " bytes, the most any load target holds)");
        data.insert(data.end(), buf, buf + n);
    }
    if (std::ferror(f.get())) throw LoadError("cannot read " + name + ": " + std::strerror(errno));
    if (data.empty()) throw LoadError(name + " is empty");
    return data;
}

// An unsigned number with an optional 0x, 0o or 0b prefix; nullopt if `s` is
// not one (or does not fit in 64 bits).
std::optional<uint64_t> number(const std::string &s) {
    unsigned radix = 10;
    size_t   i     = 0;
    if (s.size() > 2 && s[0] == '0') {
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(s[1])));
        if (p == 'x') radix = 16;
        if (p == 'o') radix = 8;
        if (p == 'b') radix = 2;
        if (radix != 10) i = 2;
    }
    if (i == s.size()) return std::nullopt;
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        char     c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        unsigned d;
        if (c >= '0' && c <= '9')
            d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<unsigned>(c - 'a' + 10);
        else
            return std::nullopt;
        if (d >= radix || v > (UINT64_MAX - d) / radix) return std::nullopt;
        v = v * radix + d;
    }
    return v;
}

const AliasSpec *find_alias(const Machine &m, const std::string &target) {
    const std::string want = upper(target);
    for (const auto &[name, spec] : m.aliases()) {
        if (upper(name) == want) return &spec;
    }
    return nullptr;
}

LoadAction plan_module(Machine &machine, const LoadSpec &spec, const FlashAlias &alias, std::vector<uint8_t> data) {
    Module *module = machine.module(alias.module);
    if (!module || !module->loadable()) throw LoadError(alias.module + " cannot be loaded");
    if (data.size() > UINT32_MAX)
        throw LoadError(spec.file.string() + ": file is too large (" + std::to_string(data.size()) + " bytes)");
    try {
        module->check_load(alias.offset, static_cast<uint32_t>(data.size()));
    } catch (const std::invalid_argument &e) {
        throw LoadError(spec.file.string() + ": " + e.what());
    }
    return {spec.file, module, nullptr, alias.offset, std::move(data)};
}

LoadAction plan_ram(Machine &machine, const LoadSpec &spec, uint64_t addr, std::vector<uint8_t> data) {
    const std::string file = spec.file.string();
    const uint64_t    end  = addr + data.size(); // exclusive
    for (const RangeInfo &r : machine.bus().module_ranges()) {
        if (addr < uint64_t{r.base} + r.size && r.base < end)
            throw LoadError(file + ": " + hex32(addr) + ".." + hex32(end) + " lies in " + r.module + "." + r.range +
                            ", which cannot be loaded");
    }
    if (addr == 0) throw LoadError(file + ": address 0 is the flash window; load flash through an alias");
    for (const MemRegion &region : machine.bus().memory_regions()) {
        const uint64_t region_end = uint64_t{region.base} + region.size;
        if (addr < region.base || addr >= region_end) continue;
        if (end > region_end)
            throw LoadError(file + ": " + std::to_string(data.size()) + " bytes at " + hex32(addr) +
                            " does not fit: " + hex32(addr) + ".." + hex32(end) + " crosses the end of the region " +
                            region.name + " at " + hex32(region_end));
        return {spec.file, nullptr, &machine.bus(), static_cast<uint32_t>(addr), std::move(data)};
    }
    throw LoadError(file + ": address " + hex32(addr) + " is outside memory");
}

} // namespace

LoadSpec parse_load(std::string_view arg) {
    auto colon = arg.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == arg.size())
        throw LoadError("--load expects target:file, got " + std::string(arg));
    return {std::string(arg.substr(0, colon)), std::filesystem::path(arg.substr(colon + 1))};
}

std::vector<LoadAction> plan_loads(Machine &machine, const std::vector<LoadSpec> &loads) {
    std::vector<LoadAction> actions;
    const uint64_t          cap = max_capacity(machine);
    for (const LoadSpec &spec : loads) {
        std::vector<uint8_t>    data = read_file(spec.file, cap);
        std::optional<uint64_t> addr;
        if (const AliasSpec *alias = find_alias(machine, spec.target)) {
            if (const auto *fa = std::get_if<FlashAlias>(&alias->target)) {
                actions.push_back(plan_module(machine, spec, *fa, std::move(data)));
                continue;
            }
            addr = std::get<uint32_t>(alias->target);
        } else {
            addr = number(spec.target);
            if (!addr) throw LoadError("unknown load target '" + spec.target + "' (not an alias or a number)");
        }
        actions.push_back(plan_ram(machine, spec, *addr, std::move(data)));
    }
    return actions;
}

void apply(const std::vector<LoadAction> &actions) {
    for (const LoadAction &a : actions) {
        if (a.module)
            a.module->load(a.at, a.data);
        else
            a.bus->write_memory(a.at, a.data);
    }
}

} // namespace bk
