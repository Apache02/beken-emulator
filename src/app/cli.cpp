#include "app/cli.h"

#include <optional>
#include <string_view>

namespace bk {

std::string usage() {
    return "Usage: bkemu PROFILE --load TARGET:FILE [--load TARGET:FILE]... [options]\n"
           "\n"
           "Runs firmware in an emulated BK7252. Prints the host end of each UART\n"
           "(for example `UART2: /dev/pts/7`) and runs until SIGINT or SIGTERM.\n"
           "\n"
           "Arguments:\n"
           "  PROFILE              Chip profile name in upper case (for example BK7252);\n"
           "                       read from <PROFILE>.toml in the profile directory\n"
           "\n"
           "Options:\n"
           "  --load TARGET:FILE   Load an image before start; repeatable, at least one is\n"
           "                       required. TARGET is a profile alias (for example\n"
           "                       bootloader, app, RAM, IRAM) or an address\n"
           "  -d, --debug LIST     Enable debug logging for the comma-separated logger\n"
           "                       names, or `all`; repeatable\n"
           "  --uart1 SPEC         Host end of UART1: pty (default) or stdio\n"
           "  --uart2 SPEC         Host end of UART2: pty (default) or stdio\n"
           "  -h, --help           Show this help and exit\n"
           "\n"
           "Environment:\n"
           "  BK7252_PROFILES      Directory with the profile files\n"
           "                       (default: <executable directory>/../profiles)\n"
           "  NO_COLOR             Turn log colour off\n"
           "  FORCE_COLOR          Turn log colour on even when stderr is not a terminal\n";
}

CliOptions parse_cli(int argc, char **argv) {
    CliOptions o;
    bool       have_profile = false;
    for (int i = 1; i < argc; ++i) {
        std::string                arg = argv[i];
        std::string                opt = arg;
        std::optional<std::string> inline_value;
        if (arg.starts_with("--")) {
            if (auto eq = arg.find('='); eq != std::string::npos) {
                opt          = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
            }
        }
        auto value = [&]() -> std::string {
            if (inline_value) return *inline_value;
            if (i + 1 >= argc) throw CliError(opt + " needs a value");
            return argv[++i];
        };

        if (opt == "-h" || opt == "--help") {
            o.help = true;
            return o;
        }
        if (opt == "-p" || opt == "--publish") throw CliError("--publish is not supported in this build");
        if (opt == "--i2c1") throw CliError("--i2c1 is not supported in this build");
        if (opt == "--load") {
            try {
                o.loads.push_back(parse_load(value()));
            } catch (const LoadError &e) {
                throw CliError(e.what());
            }
        } else if (opt == "-d" || opt == "--debug") {
            std::string csv   = value();
            size_t      start = 0;
            while (start <= csv.size()) {
                size_t comma = csv.find(',', start);
                if (comma == std::string::npos) comma = csv.size();
                if (comma > start) o.debug.push_back(csv.substr(start, comma - start));
                start = comma + 1;
            }
        } else if (opt == "--uart1") {
            o.uart1 = value();
        } else if (opt == "--uart2") {
            o.uart2 = value();
        } else if (arg.size() > 1 && arg[0] == '-') {
            throw CliError("unknown option " + opt);
        } else if (have_profile) {
            throw CliError("unexpected argument " + arg);
        } else {
            o.profile    = arg;
            have_profile = true;
        }
    }
    if (!have_profile) throw CliError("a profile name is required");
    if (o.loads.empty()) throw CliError("at least one --load is required");
    return o;
}

} // namespace bk
