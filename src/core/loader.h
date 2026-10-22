#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/machine.h"

namespace bk {

// An image that cannot be loaded.
struct LoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One `--load TARGET:FILE`.
struct LoadSpec {
    std::string           target;
    std::filesystem::path file;
};

// Splits at the first ':'; both parts must be non-empty. Throws LoadError
// `--load expects target:file, got X`.
LoadSpec parse_load(std::string_view arg);

// A checked load: into a loadable module at `at` (an offset) when `module`
// is set, otherwise into RAM through `bus` at address `at`.
struct LoadAction {
    std::filesystem::path file;
    Module               *module = nullptr;
    Bus                  *bus    = nullptr;
    uint32_t              at     = 0;
    std::vector<uint8_t>  data;
};

// Reads and checks every load before anything is written. The target is an
// alias of the machine (case-insensitive) or a number (0x hex, 0o, 0b or
// decimal). An alias { module, offset } goes to that module's load(); a
// number must lie inside one RAM region and outside every module range;
// address 0 (the flash window) is always refused.
//
// Throws LoadError: `cannot read X: <reason>`, `X is empty`, `unknown load
// target 'x' (not an alias or a number)`, `<module> cannot be loaded`,
// `<file>: <module refusal>`, `<file>: 0x..0x.. lies in M.r, which cannot be
// loaded`, `<file>: address 0x... is outside memory`, `<file>: N bytes at
// 0x... does not fit: ... crosses the end of the region R`.
std::vector<LoadAction> plan_loads(Machine &machine, const std::vector<LoadSpec> &loads);

// Writes every planned load, in order.
void apply(const std::vector<LoadAction> &actions);

} // namespace bk
