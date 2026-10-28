#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include "app/cli.h"

using namespace bk;

namespace {

CliOptions parse(std::vector<std::string> args) {
    args.insert(args.begin(), "bk7252-emu");
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    return parse_cli(static_cast<int>(args.size()), argv.data());
}

std::string error_of(std::vector<std::string> args) {
    try {
        parse(std::move(args));
    } catch (const CliError &e) {
        return e.what();
    }
    return "(no error)";
}

} // namespace

TEST_CASE("cli help needs no profile or load") {
    CHECK(parse({"--help"}).help);
    CHECK(parse({"-h"}).help);
    CHECK(parse({"BK7252", "--help"}).help);
    CHECK(parse({"BK7252", "--load", "app:a.bin", "-h"}).help);
    CHECK_FALSE(parse({"BK7252", "--load", "app:a.bin"}).help);
}

TEST_CASE("cli help stops parsing at once") {
    // Nothing after the help flag is looked at, so a bad option does not hide the help.
    CHECK(parse({"--help", "--frobnicate"}).help);
}

TEST_CASE("usage text lists every option") {
    std::string u = usage();
    CHECK(u.starts_with("Usage: bkemu PROFILE --load TARGET:FILE"));
    for (const char *needle : {"--load",
                               "--log-level NAME=LEVEL",
                               "--uart NAME=SPEC",
                               "--uart1",
                               "--uart2",
                               "--i2c NAME=",
                               "--i2c1",
                               "--i2c2",
                               "-p, --publish SPEC",
                               "[HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp]",
                               "--ssid NAME",
                               "--wifi-ap SSID",
                               "--wifi-password PASS",
                               "--wifi-channel N",
                               "--wifi-internet",
                               "--pcap FILE",
                               "--flash-mount [NAME=]FILE",
                               "--print-config",
                               "-h, --help",
                               "BKEMU_PROFILES"})
        CHECK_MESSAGE(u.find(needle) != std::string::npos, needle);
    CHECK(u.back() == '\n');
}

TEST_CASE("cli print-config is a flag and still needs a profile and a load") {
    CHECK_FALSE(parse({"BK7252", "--load", "app:a.bin"}).print_config);
    CHECK(parse({"BK7252", "--load", "app:a.bin", "--print-config"}).print_config);
    CHECK(error_of({"BK7252", "--print-config"}) == "at least one --load is required");
}

TEST_CASE("cli requires load") {
    CHECK(error_of({"BK7252"}) == "at least one --load is required");
    CHECK(error_of({"BK7252", "--log-level", "bus=debug"}) == "at least one --load is required");
}

TEST_CASE("cli parses profile, loads and uarts") {
    CliOptions o = parse({"BK7252",
                          "--load",
                          "bootloader:boot.bin",
                          "--load=app:C:\\app.bin",
                          "--uart1",
                          "stdio",
                          "--uart2=/dev/ttyUSB0"});
    CHECK(o.profile == "BK7252");
    REQUIRE(o.loads.size() == 2);
    CHECK(o.loads[0].target == "bootloader");
    CHECK(o.loads[0].file == "boot.bin");
    CHECK(o.loads[1].target == "app");
    CHECK(o.loads[1].file == "C:\\app.bin");
    CHECK(o.uart == std::map<std::string, std::string>{
                        {"UART1", "stdio"       },
                        {"UART2", "/dev/ttyUSB0"}
    });
    CHECK(o.log_levels.empty());

    CliOptions d = parse({"--load", "ram:x.bin", "BK7252"});
    CHECK(d.profile == "BK7252");
    CHECK(d.uart.empty());
}

TEST_CASE("cli uart takes any module name, the last one for a name wins") {
    CliOptions o =
        parse({"BK7252", "--load", "ram:x.bin", "--uart", "CONSOLE=stdio", "--uart=AUX=pty", "--uart1", "a"});
    CHECK(o.uart == std::map<std::string, std::string>{
                        {"AUX",     "pty"  },
                        {"CONSOLE", "stdio"},
                        {"UART1",   "a"    }
    });
    CliOptions again = parse({"BK7252", "--load", "ram:x.bin", "--uart1", "a", "--uart", "UART1=b"});
    CHECK(again.uart.at("UART1") == "b");
    // The spec may itself contain '='.
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--uart", "U=/dev/x=y"}).uart.at("U") == "/dev/x=y");
}

TEST_CASE("cli uart errors") {
    for (const char *bad : {"nospec", "=stdio", "UART1=", "="})
        CHECK_MESSAGE(error_of({"BK7252", "--load", "ram:x.bin", "--uart", bad}) == "--uart needs NAME=SPEC", bad);
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--uart"}) == "--uart needs a value");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--uart1"}) == "--uart1 needs a value");
}

TEST_CASE("cli log levels are repeatable and keep their order") {
    CliOptions o = parse({"BK7252",
                          "--load",
                          "ram:x.bin",
                          "--log-level",
                          "all=DEBUG",
                          "--log-level",
                          "unknown=warning",
                          "--log-level=net.stack=Error"});
    CHECK(o.log_levels == std::vector<std::pair<std::string, Level>>{
                              {"all",       Level::Debug  },
                              {"unknown",   Level::Warning},
                              {"net.stack", Level::Error  }
    });
}

TEST_CASE("cli log level errors") {
    const std::vector<std::string> base{"BK7252", "--load", "ram:x.bin"};
    auto                           error = [&](const std::string &spec) {
        std::vector<std::string> args = base;
        args.push_back("--log-level");
        args.push_back(spec);
        return error_of(args);
    };
    CHECK(error("bus") == "--log-level needs NAME=LEVEL");
    CHECK(error("=debug") == "--log-level needs NAME=LEVEL");
    CHECK(error("bus=") == "--log-level needs NAME=LEVEL");
    CHECK(error("bus=loud") == "bad log level 'loud' (want debug, info, warning, error or none)");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--log-level"}) == "--log-level needs a value");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "-d", "bus"}) == "unknown option -d");
}

TEST_CASE("cli errors") {
    CHECK(error_of({"--load", "ram:x.bin"}) == "a profile name is required");
    CHECK(error_of({"BK7252", "OTHER", "--load", "ram:x.bin"}) == "unexpected argument OTHER");
    CHECK(error_of({"BK7252", "--load"}) == "--load needs a value");
    CHECK(error_of({"BK7252", "--load", "nocolon"}) == "--load expects target:file, got nocolon");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--frobnicate"}) == "unknown option --frobnicate");
}

TEST_CASE("cli publish is repeatable, in command-line order") {
    CliOptions o = parse({"BK7252",
                          "--load",
                          "ram:x.bin",
                          "-p",
                          "8080:80",
                          "--publish",
                          "127.0.0.2:5353:53/udp",
                          "--publish=9000:9001/tcp"});
    REQUIRE(o.publish.size() == 3);
    CHECK(o.publish[0].host_ip == "127.0.0.1");
    CHECK(o.publish[0].host_port == 8080);
    CHECK(o.publish[0].guest_port == 80);
    CHECK_FALSE(o.publish[0].udp);
    CHECK(o.publish[1].host_ip == "127.0.0.2");
    CHECK(o.publish[1].host_port == 5353);
    CHECK(o.publish[1].guest_port == 53);
    CHECK(o.publish[1].udp);
    CHECK(o.publish[2].host_port == 9000);
    CHECK(parse({"BK7252", "--load", "ram:x.bin"}).publish.empty());
}

TEST_CASE("cli publish errors carry the request") {
    const std::string want = " (want [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp], ports 1-65535)";
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "-p", "80"}) == "bad --publish '80'" + want);
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--publish", "0:80"}) == "bad --publish '0:80'" + want);
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "-p", "80:80/sctp"}) == "bad --publish '80:80/sctp'" + want);
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "-p"}) == "-p needs a value");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--publish"}) == "--publish needs a value");
}

TEST_CASE("cli ssid") {
    CHECK_FALSE(parse({"BK7252", "--load", "ram:x.bin"}).ssid);
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--ssid", "my cam"}).ssid == "my cam");
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--ssid=a=b"}).ssid == "a=b");
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--ssid", std::string(32, 'x')}).ssid == std::string(32, 'x'));
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--ssid"}) == "--ssid needs a value");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--ssid", ""}) == "--ssid needs a name of 1 to 32 bytes");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--ssid", std::string(33, 'x')}) ==
          "--ssid needs a name of 1 to 32 bytes");
}

TEST_CASE("cli wifi ap options") {
    CliOptions none = parse({"BK7252", "--load", "ram:x.bin"});
    CHECK_FALSE(none.wifi_ap);
    CHECK_FALSE(none.wifi_password);
    CHECK(none.wifi_channel == 1);
    CHECK_FALSE(none.wifi_internet);
    CHECK_FALSE(parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n"}).wifi_internet);
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n", "--wifi-internet"}).wifi_internet);

    CliOptions o = parse({"BK7252",
                          "--load",
                          "ram:x.bin",
                          "--wifi-ap",
                          "TESTNET",
                          "--wifi-password",
                          "secret12",
                          "--wifi-channel",
                          "6"});
    CHECK(o.wifi_ap == "TESTNET");
    CHECK(o.wifi_password == "secret12");
    CHECK(o.wifi_channel == 6);

    CliOptions e = parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap=a=b", "--wifi-channel=13"});
    CHECK(e.wifi_ap == "a=b");
    CHECK_FALSE(e.wifi_password);
    CHECK(e.wifi_channel == 13);
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap", std::string(32, 'x')}).wifi_ap == std::string(32, 'x'));
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n", "--wifi-password", std::string(63, 'p')})
              .wifi_password == std::string(63, 'p'));
    // -p works in the AP role too.
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n", "-p", "8080:80"}).publish.size() == 1);
}

TEST_CASE("cli pcap") {
    CHECK_FALSE(parse({"BK7252", "--load", "ram:x.bin"}).pcap);
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n", "--pcap", "a.cap"}).pcap == "a.cap");
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "--ssid", "n", "--pcap=b.cap"}).pcap == "b.cap");
    CHECK(parse({"BK7252", "--load", "ram:x.bin", "-p", "8080:80", "--pcap", "c.cap"}).pcap == "c.cap");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n", "--pcap", ""}) == "--pcap needs a file name");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--wifi-ap", "n", "--pcap"}) == "--pcap needs a value");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--pcap", "a.cap"}) ==
          "--pcap needs --wifi-ap, --ssid or --publish");
}

TEST_CASE("cli wifi ap errors") {
    auto error = [](std::vector<std::string> extra) {
        std::vector<std::string> args{"BK7252", "--load", "ram:x.bin"};
        args.insert(args.end(), extra.begin(), extra.end());
        return error_of(args);
    };
    CHECK(error({"--wifi-ap"}) == "--wifi-ap needs a value");
    CHECK(error({"--wifi-ap", ""}) == "--wifi-ap needs an SSID of 1 to 32 bytes");
    CHECK(error({"--wifi-ap", std::string(33, 'x')}) == "--wifi-ap needs an SSID of 1 to 32 bytes");
    CHECK(error({"--wifi-ap", "n", "--wifi-password", "short12"}) == "--wifi-password needs 8 to 63 characters");
    CHECK(error({"--wifi-ap", "n", "--wifi-password", std::string(64, 'p')}) ==
          "--wifi-password needs 8 to 63 characters");
    for (const char *bad : {"0", "14", "x", "", "6x", "-1", "99999999999"})
        CHECK_MESSAGE(error({"--wifi-ap", "n", "--wifi-channel", bad}) == "--wifi-channel needs a channel 1-13", bad);
    CHECK(error({"--wifi-password", "secret12"}) ==
          "--wifi-password, --wifi-channel and --wifi-internet need --wifi-ap");
    CHECK(error({"--wifi-channel", "6"}) == "--wifi-password, --wifi-channel and --wifi-internet need --wifi-ap");
    CHECK(error({"--wifi-internet"}) == "--wifi-password, --wifi-channel and --wifi-internet need --wifi-ap");
    CHECK(error({"--wifi-ap", "n", "--ssid", "cam"}) == "--wifi-ap and --ssid exclude each other");
    CHECK(error({"--ssid", "cam", "--wifi-ap", "n"}) == "--wifi-ap and --ssid exclude each other");
}

TEST_CASE("cli i2c takes NAME=DEVICE, --i2c1 is a shorthand, order is kept") {
    CliOptions o = parse(
        {"BK7252", "--load", "ram:x.bin", "--i2c1", "hi704", "--i2c", "BUS2=hi704@0x31", "--i2c=I2C1=hi704@0x32"});
    CHECK(o.i2c == std::vector<std::pair<std::string, std::string>>{
                       {"I2C1", "hi704"     },
                       {"BUS2", "hi704@0x31"},
                       {"I2C1", "hi704@0x32"}
    });
    CHECK(parse({"BK7252", "--load", "ram:x.bin"}).i2c.empty());
}

TEST_CASE("cli i2c errors") {
    for (const char *bad : {"nodevice", "=hi704", "I2C1=", "="})
        CHECK_MESSAGE(error_of({"BK7252", "--load", "ram:x.bin", "--i2c", bad}) == "--i2c needs NAME=TYPE[@ADDRESS]",
                      bad);
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--i2c"}) == "--i2c needs a value");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--i2c1"}) == "--i2c1 needs a value");
}

TEST_CASE("cli flash mount takes FILE or NAME=FILE, order is kept") {
    CliOptions o = parse({"BK7252",
                          "--load",
                          "ram:x.bin",
                          "--flash-mount",
                          "disk.bin",
                          "--flash-mount=FLASH2=b.bin",
                          "--flash-mount",
                          "dir/a=b.bin",
                          "--flash-mount",
                          "=c.bin"});
    CHECK(o.flash_mounts == std::vector<std::pair<std::string, std::string>>{
                                {"",       "disk.bin"   },
                                {"FLASH2", "b.bin"      },
                                {"",       "dir/a=b.bin"},
                                {"",       "=c.bin"     }
    });
    CHECK(parse({"BK7252", "--load", "ram:x.bin"}).flash_mounts.empty());
}

TEST_CASE("cli flash mount errors") {
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--flash-mount", "FLASH="}) == "--flash-mount needs [NAME=]FILE");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--flash-mount", ""}) == "--flash-mount needs [NAME=]FILE");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--flash-mount"}) == "--flash-mount needs a value");
}
