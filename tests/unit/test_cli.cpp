#include <doctest/doctest.h>

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
    for (const char *needle : {"--load", "-d, --debug", "--uart1", "--uart2", "-h, --help", "BK7252_PROFILES"})
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
    CHECK(o.uart1 == "stdio");
    CHECK(o.uart2 == "/dev/ttyUSB0");
    CHECK(o.debug.empty());

    CliOptions d = parse({"--load", "ram:x.bin", "BK7252"});
    CHECK(d.profile == "BK7252");
    CHECK(d.uart1 == "pty");
    CHECK(d.uart2 == "pty");
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
    CHECK(error_of({"BK7252", "--load", "ram:x.bin", "--i2c1", "ov7670"}) == "--i2c1 is not supported in this build");
}
