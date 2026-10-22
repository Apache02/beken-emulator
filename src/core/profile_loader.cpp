#include "core/profile_loader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <toml++/toml.hpp>

namespace bk {

namespace {

// A problem in the profile's contents; becomes `<file>: bad profile: <detail>`.
struct Bad {
    std::string detail;
};

std::string hex32(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%08llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Typed access to the keys of one table; `where` prefixes error details.
struct Fields {
    const toml::table &table;
    std::string        where;

    std::string prefix() const { return where.empty() ? "" : where + ": "; }

    const toml::node *get(const std::string &key, bool required) const {
        const toml::node *n = table.get(key);
        if (!n && required) throw Bad{prefix() + "missing key '" + key + "'"};
        return n;
    }
    int64_t integer(const std::string &key) const {
        const toml::node *n = get(key, true);
        if (!n->is_integer()) throw Bad{prefix() + key + " must be an integer"};
        return n->as_integer()->get();
    }
    uint32_t u32(const std::string &key) const {
        int64_t v = integer(key);
        if (v < 0 || v > 0xFFFFFFFFLL) throw Bad{prefix() + key + " must fit in 32 bits"};
        return static_cast<uint32_t>(v);
    }
    std::string string(const std::string &key) const {
        const toml::node *n = get(key, true);
        if (!n->is_string()) throw Bad{prefix() + key + " must be a string"};
        return n->as_string()->get();
    }
    const toml::table *table_at(const std::string &key, bool required) const {
        const toml::node *n = get(key, required);
        if (!n) return nullptr;
        if (!n->is_table()) throw Bad{prefix() + key + " must be a table"};
        return n->as_table();
    }
    // Each element of the array of tables `key` (absent: none).
    std::vector<const toml::table *> tables(const std::string &key) const {
        std::vector<const toml::table *> out;
        const toml::node                *n = get(key, false);
        if (!n) return out;
        const toml::array *arr = n->as_array();
        if (!arr) throw Bad{prefix() + key + " must be an array of tables"};
        for (const toml::node &el : *arr) {
            if (!el.is_table()) throw Bad{prefix() + key + " must be an array of tables"};
            out.push_back(el.as_table());
        }
        return out;
    }
};

ParamValue param(const toml::node &n, const std::string &where);

ParamTable params(const toml::table &t, const std::string &where) {
    ParamTable out;
    for (const auto &[key, value] : t) {
        std::string k(key.str());
        out.set(k, param(value, where + "." + k));
    }
    return out;
}

ParamValue param(const toml::node &n, const std::string &where) {
    if (n.is_integer()) return n.as_integer()->get();
    if (n.is_string()) return n.as_string()->get();
    if (n.is_boolean()) return static_cast<int64_t>(n.as_boolean()->get() ? 1 : 0);
    if (n.is_table()) return params(*n.as_table(), where);
    if (const toml::array *arr = n.as_array()) {
        ParamList list;
        for (const toml::node &el : *arr) {
            if (!el.is_integer()) throw Bad{where + " must be a list of integers"};
            list.push_back(el.as_integer()->get());
        }
        return list;
    }
    throw Bad{where + " has an unsupported type"};
}

MemSpec memory(const toml::table &t, size_t index) {
    MemSpec m;
    m.name = Fields{t, "memory[" + std::to_string(index) + "]"}.string("name");
    Fields f{t, "memory " + m.name};
    m.base = f.u32("base");
    m.size = f.u32("size");
    if (f.get("reset_fill", false)) {
        int64_t fill = f.integer("reset_fill");
        if (fill < 0 || fill > 255) throw Bad{f.prefix() + "reset_fill must be a byte 0-255"};
        m.reset_fill = static_cast<uint8_t>(fill);
    }
    if (const toml::node *x = f.get("executable", false)) {
        if (!x->is_boolean()) throw Bad{f.prefix() + "executable must be true or false"};
        m.executable = x->as_boolean()->get();
    }
    return m;
}

ModuleSpec module(const toml::table &t, size_t index) {
    ModuleSpec m;
    Fields     first{t, "module[" + std::to_string(index) + "]"};
    m.type = first.string("type");
    m.name = first.string("name");
    Fields f{t, "module " + m.name};
    if (const toml::table *bases = f.table_at("bases", false)) {
        Fields b{*bases, "module " + m.name + " bases"};
        for (const auto &[key, value] : *bases) {
            std::string range(key.str());
            m.bases[range] = b.u32(range);
        }
    }
    if (const toml::table *p = f.table_at("params", false)) m.params = params(*p, "module " + m.name + " params");
    return m;
}

void aliases(const toml::table &t, Profile &p) {
    std::set<std::string> modules;
    for (const auto &m : p.modules) modules.insert(m.name);
    Fields f{t, "alias"};
    for (const auto &[key, value] : t) {
        std::string raw(key.str()), name = upper(raw);
        if (p.alias.count(name)) throw Bad{"duplicate alias " + name};
        if (value.is_integer()) {
            uint32_t addr   = f.u32(raw);
            bool     inside = std::any_of(p.memory.begin(), p.memory.end(), [&](const MemSpec &m) {
                return addr >= m.base && addr - m.base < m.size;
            });
            if (!inside) throw Bad{"alias " + name + " is outside memory: " + hex32(addr)};
            p.alias[name] = {addr};
        } else if (const toml::table *target = value.as_table()) {
            Fields     a{*target, "alias " + name};
            FlashAlias fa{a.string("module"), a.u32("offset")};
            if (!modules.count(fa.module)) throw Bad{"alias " + name + " names unknown module " + fa.module};
            p.alias[name] = {fa};
        } else {
            throw Bad{"alias " + name + " must be an address or { module, offset }"};
        }
    }
}

Profile build(const toml::table &root) {
    Fields  f{root, ""};
    Profile p;
    p.name                 = f.string("name");
    p.insns_per_us         = f.u32("insns_per_us");
    const toml::node  *win = f.get("peripheral_window", true);
    const toml::array *w   = win->as_array();
    if (!w || w->size() != 2 || !(*w)[0].is_integer() || !(*w)[1].is_integer())
        throw Bad{"peripheral_window must be [base, size]"};
    int64_t base = (*w)[0].as_integer()->get(), size = (*w)[1].as_integer()->get();
    if (base < 0 || base > 0xFFFFFFFFLL || size < 0 || size > 0xFFFFFFFFLL)
        throw Bad{"peripheral_window must fit in 32 bits"};
    p.win_base = static_cast<uint32_t>(base);
    p.win_size = static_cast<uint32_t>(size);

    auto mems = f.tables("memory");
    for (size_t i = 0; i < mems.size(); ++i) p.memory.push_back(memory(*mems[i], i));
    auto mods = f.tables("module");
    for (size_t i = 0; i < mods.size(); ++i) p.modules.push_back(module(*mods[i], i));
    if (const toml::table *a = f.table_at("alias", false)) aliases(*a, p);
    return p;
}

} // namespace

Profile load_profile(const std::string &name, const std::filesystem::path &dir) {
    if (name.empty() || name != upper(name)) throw ProfileError("profile name must be upper case");
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            throw ProfileError("profile name must contain only A-Z, 0-9, '_' and '-': " + name);
    }
    std::filesystem::path file = dir / (name + ".toml");
    std::error_code       ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        std::vector<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".toml") names.push_back(entry.path().stem().string());
        }
        std::sort(names.begin(), names.end());
        std::string list;
        for (const auto &n : names) list += (list.empty() ? "" : ", ") + n;
        throw ProfileError("no profile " + name + " (available: " + (list.empty() ? "none" : list) + ")");
    }
    const std::string prefix = file.string() + ": bad profile: ";
    try {
        toml::table root = toml::parse_file(file.string());
        return build(root);
    } catch (const toml::parse_error &e) {
        throw ProfileError(prefix + std::string(e.description()) + " (line " + std::to_string(e.source().begin.line) +
                           ", column " + std::to_string(e.source().begin.column) + ")");
    } catch (const Bad &b) {
        throw ProfileError(prefix + b.detail);
    }
}

std::filesystem::path profile_dir(const std::filesystem::path &exe) {
    const char *env = std::getenv("BK7252_PROFILES");
    if (env && *env) return env;
    return exe.parent_path().parent_path() / "profiles";
}

} // namespace bk
