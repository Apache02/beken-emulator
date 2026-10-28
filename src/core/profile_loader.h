#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/profile.h"

namespace bk {

// A profile that cannot be found or read.
struct ProfileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Reads `<dir>/<name>.toml`.
//
// Top-level keys: name (string), insns_per_us (integer), peripheral_window
// ([base, size]), [[memory]] (name, base, size, optional reset_fill 0-255,
// optional executable = true), [alias] (NAME = address inside a memory region,
// or NAME = { module, offset } naming a listed module) and [[module]] (type,
// name, [module.bases], [module.params]). Unknown keys are ignored. Alias names
// are stored upper case. Module parameters keep integers, strings, booleans
// (as 0/1), tables and lists of integers; table keys come in sorted order.
//
// `extends = "OTHER"` takes the profile OTHER as the base: the keys of the
// extending file replace those of the base, `[alias]` and the tables inside an
// entry (`[module.bases]`, `[module.params]`) merge key by key, and a
// `[[memory]]` or `[[module]]` entry with the name of one of the base changes it
// (its keys merge into it) while a new name is added at the end. The profile is
// called after its file unless it has a `name`. The chain can be any length.
//
// Throws ProfileError: `profile name must be upper case`, `no profile X
// (available: A, B)`, or `<file>: bad profile: <detail>` for syntax errors,
// missing keys, values of the wrong type and an `extends` cycle.
Profile load_profile(const std::string &name, const std::filesystem::path &dir);

// The profile directory: $BKEMU_PROFILES when set and non-empty, otherwise
// `profiles` next to the directory holding the executable
// (<dir of exe>/../profiles).
std::filesystem::path profile_dir(const std::filesystem::path &exe);

} // namespace bk
