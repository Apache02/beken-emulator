#pragma once

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
    std::vector<LoadSpec>    loads;                        // command-line order
    std::vector<std::string> debug;                        // logger suffixes, or "all"
    std::string              uart1 = "pty", uart2 = "pty"; // pty, stdio or a device path
    bool                     help = false;                 // -h/--help: print usage() and exit; nothing else is set
};

// PROFILE --load TARGET:FILE [--load ...] [-d LIST]... [--uart1 SPEC] [--uart2 SPEC]
//
// Long options also take `--opt=value`. `-d`/`--debug` is repeatable and takes
// a comma-separated list. Throws CliError, e.g. `at least one --load is
// required`; `-p`/`--publish` and `--i2c1` are refused with `... is not
// supported in this build`. `-h`/`--help` stops parsing at once and returns
// with `help` set; no profile or load is needed then.
CliOptions parse_cli(int argc, char **argv);

// The text printed for --help; ends with a newline.
std::string usage();

} // namespace bk
