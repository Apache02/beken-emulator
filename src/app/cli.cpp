#include "app/cli.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

namespace bk {

namespace {

// The NAME of a NAME=VALUE: the text before the first '=' when it is a module
// name (letters, digits, underscores), otherwise empty.
std::string name_prefix(const std::string &v) {
    auto eq = v.find('=');
    if (eq == std::string::npos || eq == 0) return {};
    for (size_t i = 0; i < eq; ++i)
        if (!std::isalnum(static_cast<unsigned char>(v[i])) && v[i] != '_') return {};
    return v.substr(0, eq);
}

} // namespace

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
           "  --log-level NAME=LEVEL\n"
           "                       Set the level of the logger NAME (for example bus,\n"
           "                       net.stack) or of every logger with `all`; LEVEL is\n"
           "                       debug, info, warning, error or none; repeatable\n"
           "  --uart NAME=SPEC     Host end of the UART module NAME from the profile:\n"
           "                       pty (default), stdio or a serial device path such as\n"
           "                       /dev/ttyUSB0 (115200 baud); repeatable\n"
           "  --uart1 SPEC         Same as --uart UART1=SPEC\n"
           "  --uart2 SPEC         Same as --uart UART2=SPEC\n"
           "  --i2c NAME=DEVICE    Attach a device to the I2C controller module NAME of the\n"
           "                       profile; DEVICE is TYPE or TYPE@ADDRESS (7-bit, 0x08-0x77),\n"
           "                       for example hi704; repeatable\n"
           "  --i2c1 DEVICE        Same as --i2c I2C1=DEVICE\n"
           "  --flash-mount [NAME=]FILE\n"
           "                       Keep the flash in FILE, like a virtual machine disk:\n"
           "                       created with the size of the flash if missing, every\n"
           "                       --load and every write of the firmware goes to it at\n"
           "                       once. The image has the CRC bytes, as the _crc.bin\n"
           "                       files. NAME picks a flash module when the profile\n"
           "                       has several; repeatable for different modules\n"
           "  -p, --publish SPEC   Forward a host port to a port of the firmware on its WiFi\n"
           "                       network: [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp]\n"
           "                       (host address 127.0.0.1 and tcp by default; ports\n"
           "                       1-65535); needs a MAC module; repeatable\n"
           "  --ssid NAME          Join only the access point NAME (default: any open\n"
           "                       network); also connects the WiFi MAC to the host\n"
           "  --wifi-ap SSID       Run a virtual access point SSID (1-32 bytes) for\n"
           "                       firmware that joins a network as a station, with a\n"
           "                       router behind it: DHCP gives the firmware 10.0.2.15,\n"
           "                       the gateway is 10.0.2.2; excludes --ssid; needs a MAC\n"
           "                       module\n"
           "  --wifi-password PASS The access point's WPA2-PSK passphrase (8-63\n"
           "                       characters); without it the network is open\n"
           "  --wifi-channel N     The access point's channel, 1-13 (default 1)\n"
           "  --wifi-internet      Let the firmware reach the internet through the router\n"
           "                       (NAT to the host's network); without it only the LAN,\n"
           "                       ping and published ports work; needs --wifi-ap\n"
           "  --pcap FILE          Record the WiFi frames (802.11 with radiotap) in FILE as\n"
           "                       pcap; FILE may be a named pipe (mkfifo) that Wireshark\n"
           "                       reads live: wireshark -k -i FILE; needs --wifi-ap,\n"
           "                       --ssid or --publish\n"
           "  -h, --help           Show this help and exit\n"
           "\n"
           "Environment:\n"
           "  BKEMU_PROFILES       Directory with the profile files\n"
           "                       (default: <executable directory>/../profiles)\n"
           "  NO_COLOR             Turn log colour off\n"
           "  FORCE_COLOR          Turn log colour on even when stderr is not a terminal\n";
}

CliOptions parse_cli(int argc, char **argv) {
    CliOptions o;
    bool       have_profile = false;
    bool       wifi_channel = false; // --wifi-channel was given
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
        if (opt == "-p" || opt == "--publish") {
            try {
                o.publish.push_back(parse_publish(value()));
            } catch (const ForwardError &e) {
                throw CliError(e.what());
            }
        } else if (opt == "--ssid") {
            std::string name = value();
            // An 802.11 SSID is 1 to 32 bytes.
            if (name.empty() || name.size() > 32) throw CliError("--ssid needs a name of 1 to 32 bytes");
            o.ssid = std::move(name);
        } else if (opt == "--wifi-ap") {
            std::string name = value();
            if (name.empty() || name.size() > 32) throw CliError("--wifi-ap needs an SSID of 1 to 32 bytes");
            o.wifi_ap = std::move(name);
        } else if (opt == "--wifi-password") {
            // A WPA2 passphrase is 8 to 63 characters.
            std::string pass = value();
            if (pass.size() < 8 || pass.size() > 63) throw CliError("--wifi-password needs 8 to 63 characters");
            o.wifi_password = std::move(pass);
        } else if (opt == "--pcap") {
            std::string file = value();
            if (file.empty()) throw CliError("--pcap needs a file name");
            o.pcap = std::move(file);
        } else if (opt == "--wifi-internet") {
            o.wifi_internet = true;
        } else if (opt == "--wifi-channel") {
            std::string v = value();
            if (v.empty() || v.size() > 2 ||
                !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
                throw CliError("--wifi-channel needs a channel 1-13");
            unsigned channel = unsigned(std::stoul(v));
            if (channel < 1 || channel > 13) throw CliError("--wifi-channel needs a channel 1-13");
            o.wifi_channel = channel;
            wifi_channel   = true;
        } else if (opt == "--load") {
            try {
                o.loads.push_back(parse_load(value()));
            } catch (const LoadError &e) {
                throw CliError(e.what());
            }
        } else if (opt == "--log-level") {
            std::string v  = value();
            auto        eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == v.size())
                throw CliError("--log-level needs NAME=LEVEL");
            std::optional<Level> level = Logger::parse_level(std::string_view(v).substr(eq + 1));
            if (!level) throw CliError("bad log level '" + v.substr(eq + 1) + "' (want debug, info, warning, error or none)");
            o.log_levels.emplace_back(v.substr(0, eq), *level);
        } else if (opt == "--uart") {
            std::string v  = value();
            auto        eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == v.size()) throw CliError("--uart needs NAME=SPEC");
            o.uart[v.substr(0, eq)] = v.substr(eq + 1);
        } else if (opt == "--i2c") {
            std::string v  = value();
            auto        eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == v.size())
                throw CliError("--i2c needs NAME=TYPE[@ADDRESS]");
            o.i2c.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (opt == "--flash-mount") {
            std::string v    = value();
            std::string name = name_prefix(v);
            std::string file = name.empty() ? v : v.substr(name.size() + 1);
            if (file.empty()) throw CliError("--flash-mount needs [NAME=]FILE");
            o.flash_mounts.emplace_back(std::move(name), std::move(file));
        } else if (opt == "--i2c1") {
            o.i2c.emplace_back("I2C1", value());
        } else if (opt == "--uart1" || opt == "--uart2") {
            o.uart["UART" + opt.substr(6)] = value();
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
    if ((o.wifi_password || wifi_channel || o.wifi_internet) && !o.wifi_ap)
        throw CliError("--wifi-password, --wifi-channel and --wifi-internet need --wifi-ap");
    if (o.wifi_ap && o.ssid) throw CliError("--wifi-ap and --ssid exclude each other");
    if (o.pcap && !o.wifi_ap && !o.ssid && o.publish.empty())
        throw CliError("--pcap needs --wifi-ap, --ssid or --publish");
    return o;
}

} // namespace bk
