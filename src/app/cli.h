#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/loader.h"
#include "core/log.h"
#include "net/forwarder.h"

namespace bk {

// A bad command line.
struct CliError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CliOptions {
    std::string           profile;
    std::vector<LoadSpec> loads; // command-line order
    // `--log-level NAME=LEVEL` in command-line order: a logger suffix or "all", and its level.
    std::vector<std::pair<std::string, Level>> log_levels;
    // Host end per UART module name (pty, stdio or a device path). UARTs
    // without an entry get a pty; names are checked against the profile later.
    std::map<std::string, std::string> uart;
    // (I2C controller module name, device spec TYPE[@ADDRESS]) in command-line
    // order; names and specs are checked later.
    std::vector<std::pair<std::string, std::string>> i2c;
    // `-p`/`--publish`, in command-line order.
    std::vector<Publish> publish;
    // `--ssid`: the only network the virtual station joins.
    std::optional<std::string> ssid;
    bool                       help = false; // -h/--help: print usage() and exit; nothing else is set
};

// PROFILE --load TARGET:FILE [--load ...] [--log-level NAME=LEVEL]... [--uart NAME=SPEC]...
//         [--i2c NAME=TYPE[@ADDRESS]]... [-p [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp]]...
//         [--ssid NAME]
//
// `--uart1 SPEC` and `--uart2 SPEC` mean `--uart UART1=SPEC` and
// `--uart UART2=SPEC`; `--i2c1 TYPE[@ADDRESS]` means `--i2c I2C1=TYPE[@ADDRESS]`.
// Long options also take `--opt=value`. `--log-level` is repeatable; LEVEL is
// debug, info, warning or error in any letter case. Throws CliError, e.g. `at
// least one --load is required`, `--uart needs NAME=SPEC`, `--log-level needs
// NAME=LEVEL` or `--i2c needs NAME=TYPE[@ADDRESS]`; a
// bad `--publish` gives the message of parse_publish. Whether the profile has
// a MAC to publish to is checked later.
// `-h`/`--help` stops parsing at once and returns with `help` set; no profile
// or load is needed then.
CliOptions parse_cli(int argc, char **argv);

// The text printed for --help; ends with a newline.
std::string usage();

} // namespace bk
