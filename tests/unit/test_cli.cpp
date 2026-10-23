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
                               "-d, --debug",
                               "--uart NAME=SPEC",
                               "--uart1",
                               "--uart2",
                               "--i2c NAME=",
                               "--i2c1",
                               "-h, --help",
                               "BK7252_PROFILES"})
        CHECK_MESSAGE(u.find(needle) != std::string::npos, needle);
    CHECK(u.back() == '\n');
}

TEST_CASE("cli requires load") {
    CHECK(error_of({"BK7252"}) == "at least one --load is required");
    CHECK(error_of({"BK7252", "-d", "bus"}) == "at least one --load is required");
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
    CHECK(o.debug.empty());

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

TEST_CASE("cli debug csv repeatable") {
    CliOptions o = parse({"BK7252", "--load", "ram:x.bin", "-d", "bus,flash", "--debug", "wdt", "--debug=all"});
    CHECK(o.debug == std::vector<std::string>{"bus", "flash", "wdt", "all"});
}

TEST_CASE("cli errors") {
    CHECK(error_of({"--load", "ram:x.bin"}) == "a profile name is required");
    CHECK(error_of({"BK7252", "OTHER", "--load", "ram:x.bin"}) == "unexpected argument OTHER");
    CHECK(error_of({"BK7252", "--load"}) == "--load needs a value");
    CHECK(error_of({"BK7252", "--load", "nocolon"}) == "--load expects target:file, got nocolon");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--frobnicate"}) == "unknown option --frobnicate");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "-p", "8080:80"}) == "--publish is not supported in this build");
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--publish", "8080:80"}) ==
          "--publish is not supported in this build");
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
