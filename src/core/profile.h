#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bk {

struct ParamValue;

// A list of integers, e.g. a signal written as [offset, bit].
using ParamList = std::vector<int64_t>;

// Free-form module parameters: keys in the order they were given. A value is an
// integer, a string, a nested table, a list of integers or a list of tables.
struct ParamTable {
    std::vector<std::pair<std::string, ParamValue>> entries;

    ParamTable() = default;
    ParamTable(std::initializer_list<std::pair<std::string, ParamValue>> init);

    // First entry named `key`, or nullptr.
    const ParamValue *find(const std::string &key) const;
    // Replaces the entry named `key` or appends a new one.
    void set(const std::string &key, ParamValue value);
    // Removes every entry named `key`; returns whether there was one.
    bool erase(const std::string &key);
};

// A list of tables, e.g. `read_adjust = [{ offset = 8, ... }, ...]`.
using ParamTableList = std::vector<ParamTable>;

struct ParamValue : std::variant<int64_t, std::string, ParamTable, ParamList, ParamTableList> {
    using std::variant<int64_t, std::string, ParamTable, ParamList, ParamTableList>::variant;
    ParamValue(int v) : variant(static_cast<int64_t>(v)) {}
    ParamValue(const char *v) : variant(std::string(v)) {}

    const int64_t        *integer() const { return std::get_if<int64_t>(this); }
    const std::string    *string() const { return std::get_if<std::string>(this); }
    const ParamTable     *table() const { return std::get_if<ParamTable>(this); }
    const ParamList      *list() const { return std::get_if<ParamList>(this); }
    const ParamTableList *tables() const { return std::get_if<ParamTableList>(this); }
};

inline ParamTable::ParamTable(std::initializer_list<std::pair<std::string, ParamValue>> init) {
    for (const auto &[k, v] : init) set(k, v);
}

inline const ParamValue *ParamTable::find(const std::string &key) const {
    for (const auto &[k, v] : entries) {
        if (k == key) return &v;
    }
    return nullptr;
}

inline void ParamTable::set(const std::string &key, ParamValue value) {
    for (auto &[k, v] : entries) {
        if (k == key) {
            v = std::move(value);
            return;
        }
    }
    entries.emplace_back(key, std::move(value));
}

inline bool ParamTable::erase(const std::string &key) {
    auto old = entries.size();
    std::erase_if(entries, [&](const auto &e) { return e.first == key; });
    return entries.size() != old;
}

// A RAM region.
struct MemSpec {
    std::string name;
    uint32_t    base = 0, size = 0;
    // The byte the region holds after a chip reset; without it the contents
    // survive the reset.
    std::optional<uint8_t> reset_fill;
    bool                   executable = true;
};

// A point inside a module (for example a flash offset).
struct FlashAlias {
    std::string module;
    uint32_t    offset = 0;
};

// A name usable as a load target (`--load NAME:file`): a bus address or a
// point inside a module.
struct AliasSpec {
    std::variant<uint32_t, FlashAlias> target;
};

// One module instance: its type (a registry key), its name, a base address per
// range name and free-form parameters.
struct ModuleSpec {
    std::string                     type, name;
    std::map<std::string, uint32_t> bases;
    ParamTable                      params;
};

// A chip description.
struct Profile {
    std::string                      name;
    uint32_t                         insns_per_us = 100;
    uint32_t                         win_base = 0, win_size = 0; // peripheral window
    std::vector<MemSpec>             memory;
    std::map<std::string, AliasSpec> alias;
    std::vector<ModuleSpec>          modules;
};

} // namespace bk
