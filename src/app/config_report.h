#pragma once

#include <string>

#include "app/cli.h"
#include "core/machine.h"
#include "core/profile.h"

namespace bk {

// The configuration the emulator would start with, as text for `--print-config`:
// the profile and the CPU speed, then the blocks flash (one per flash module,
// with the loads that go into it), memory (with the loads that go to RAM),
// peripherals (every other module with the address and size of each range,
// log level and what is attached; sorted by address) and network. Each block
// is a tree with a header of columns; the text ends with a newline.
//
// The log level of a module is the level of the logger it writes to, so the
// `--log-level` options must have been applied (Logger::set_level) before the
// call. The machine, built from `profile` and not yet run or mounted, gives
// the ranges of the modules; no file is opened except to get the size of a
// loaded image.
//
// Throws std::invalid_argument for the names the options use and the profile
// lacks: `no UART named X in profile P`, `no I2C bus named X in profile P`,
// `no flash named X in profile P`, `profile P has more than one flash; use
// --flash-mount NAME=FILE`, `no flash in profile P`, and for a load target
// that is neither an alias nor a number: `unknown load target 'x' (not an
// alias or a number)`.
std::string config_report(const CliOptions &options, const Profile &profile, Machine &machine);

} // namespace bk
