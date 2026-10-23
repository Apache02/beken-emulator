#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/loader.h"

namespace bk {

// A bad command line.
struct CliError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CliOptions {
    std::string              profile;
    std::vector<LoadSpec>    loads; // command-line order
    std::vector<std::string> debug; // logger suffixes, or "all"
    // Host end per UART module name (pty, stdio or a device path). UARTs
    // without an entry get a pty; names are checked against the profile later.
    std::map<std::string, std::string> uart;
    // (I2C controller module name, device spec TYPE[@ADDRESS]) in command-line
    // order; names and specs are checked later.
    std::vector<std::pair<std::string, std::string>> i2c;
    bool help = false; // -h/--help: print usage() and exit; nothing else is set
};

// PROFILE --load TARGET:FILE [--load ...] [-d LIST]... [--uart NAME=SPEC]...
//         [--i2c NAME=TYPE[@ADDRESS]]...
//
// `--uart1 SPEC` and `--uart2 SPEC` mean `--uart UART1=SPEC` and
// `--uart UART2=SPEC`; `--i2c1 TYPE[@ADDRESS]` means `--i2c I2C1=TYPE[@ADDRESS]`.
// Long options also take `--opt=value`. `-d`/`--debug` is repeatable and takes
// a comma-separated list. Throws CliError, e.g. `at least one --load is
// required`, `--uart needs NAME=SPEC` or `--i2c needs NAME=TYPE[@ADDRESS]`;
// `-p`/`--publish` is refused with `... is not supported in this build`.
// `-h`/`--help` stops parsing at once and returns with `help` set; no profile
// or load is needed then.
CliOptions parse_cli(int argc, char **argv);

// The text printed for --help; ends with a newline.
std::string usage();

} // namespace bk
