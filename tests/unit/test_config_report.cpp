#include <doctest/doctest.h>

#include <stdexcept>
#include <string>

#include "app/config_report.h"
#include "app/registry.h"
#include "core/log.h"
#include "core/machine.h"
#include "core/profile_loader.h"

using namespace bk;

namespace {

Profile profile() {
    Profile p;
    p.name         = "T";
    p.insns_per_us = 100;
    p.win_base     = 0x00800000;
    p.win_size     = 0x00100000;
    p.memory.push_back({"RAM", 0x400000, 0x40000, uint8_t{0xAA}, false, 0x400000});
    p.alias["APP"] = AliasSpec{
        FlashAlias{"FLASH", 0x11000}
    };
    p.alias["IRAM"] = AliasSpec{uint32_t{0x400000}};
    p.modules.push_back({
        "flash", "FLASH", {{"xip", 0}, {"regs", 0x803000}},
          {}
    });
    p.modules.push_back({"uart", "UART1", {{"regs", 0x802100}}, {}});
    p.modules.push_back({"i2c1", "I2C1", {{"regs", 0x802300}}, {}});
    p.modules.push_back({"gdma", "GDMA", {{"regs", 0x809000}}, {}});
    p.modules.push_back({
        "wifi_mac", "MAC", {{"core", 0xC0000000}, {"pl", 0xC0008000}, {"pta", 0xC0010000}},
          {}
    });
    p.modules.push_back({"unknown", "RC", {{"regs", 0x1050000}}, {{"log_level", "debug"}}});
    return p;
}

CliOptions options() {
    CliOptions o;
    o.profile = "T";
    return o;
}

std::string report(const CliOptions &o, const Profile &p) {
    Machine machine(p, builtin_registry());
    return config_report(o, p, machine);
}

std::string error_of(const CliOptions &o, const Profile &p) {
    try {
        report(o, p);
    } catch (const std::invalid_argument &e) {
        return e.what();
    }
    return "";
}

struct Levels {
    Levels() { Logger::reset_for_tests(); }
    ~Levels() { Logger::reset_for_tests(); }
};

} // namespace

TEST_CASE("config report of a bare command line") {
    Levels     levels;
    CliOptions o = options();
    o.loads.push_back({"app", "/nonexistent/app.bin"});
    CHECK(report(o, profile()) == "profile   T\n"
                                  "cpu       100 insns/us\n"
                                  "\n"
                                  "flash\n"
                                  "│  OP    OFFSET      SIZE     FILE\n"
                                  "└─ load  0x00011000  missing  /nonexistent/app.bin\n"
                                  "\n"
                                  "memory\n"
                                  "│  NAME  ADDRESS     SIZE     NOTE\n"
                                  "└─ RAM   0x00400000  256 KiB  fill 0xAA, window 4 MiB, no exec\n"
                                  "\n"
                                  "peripherals\n"
                                  "│  NAME   TYPE      ADDRESS          SIZE    LOG    ATTACHED\n"
                                  "├─ UART1  uart      0x00802100       0x20    -      pty\n"
                                  "├─ I2C1   i2c1      0x00802300       0x8     info   -\n"
                                  "├─ FLASH  flash     0x00803000       0x28    info   -\n"
                                  "├─ GDMA   gdma      0x00809000       0x200   info   -\n"
                                  "├─ RC     unknown   0x01050000       0x100   debug  -\n"
                                  "└─ MAC    wifi_mac  core 0xC0000000  0x1580  info   -\n"
                                  "                    pl   0xC0008000  0x57C\n"
                                  "                    pta  0xC0010000  0x28\n"
                                  "\n"
                                  "network\n"
                                  "│  KEY   VALUE\n"
                                  "└─ wifi  off\n");
}

TEST_CASE("config report shows the mounted file, the RAM loads and what is attached") {
    Levels     levels;
    CliOptions o = options();
    o.flash_mounts.emplace_back("", "/data/flash.img");
    o.loads.push_back({"app", "/nonexistent/app.bin"});
    o.loads.push_back({"iram", "/nonexistent/a.bin"});
    o.loads.push_back({"0x400100", "/nonexistent/b.bin"});
    o.uart["UART1"] = "tcp:5555";
    o.i2c.emplace_back("I2C1", "hi704@0x30");
    o.i2c.emplace_back("I2C1", "eeprom");
    std::string text = report(o, profile());
    // The mounted file is the whole flash; the loads go into it.
    CHECK(text.find("flash\n"
                    "│  OP     OFFSET      SIZE     FILE\n"
                    "├─ mount  0x00000000  2 MiB    /data/flash.img\n"
                    "└─ load   0x00011000  missing  /nonexistent/app.bin\n") != std::string::npos);
    CHECK(text.find("   ├─ iram      0x00400000  missing  /nonexistent/a.bin\n") != std::string::npos);
    CHECK(text.find("   └─ 0x400100  0x00400100  missing  /nonexistent/b.bin\n") != std::string::npos);
    CHECK(text.find("tcp:5555\n") != std::string::npos);
    CHECK(text.find("hi704@0x30, eeprom\n") != std::string::npos);
}

TEST_CASE("config report takes the log levels from the loggers") {
    Levels levels;
    Logger::set_level("gdma", Level::Warning);
    Logger::set_level("i2c", Level::None);
    std::string text = report(options(), profile());
    CHECK(text.find("GDMA   gdma      0x00809000       0x200   warning  -\n") != std::string::npos);
    CHECK(text.find("I2C1   i2c1      0x00802300       0x8     none") != std::string::npos);
    // A stand-in logs at the level of its log_level parameter.
    CHECK(text.find("RC     unknown   0x01050000       0x100   debug") != std::string::npos);
}

TEST_CASE("config report of a WiFi access point, a pcap file and published ports") {
    Levels     levels;
    CliOptions o    = options();
    o.wifi_ap       = "lab";
    o.wifi_password = "secret123";
    o.wifi_channel  = 6;
    o.wifi_internet = true;
    o.pcap          = "out.pcap";
    o.publish.push_back(parse_publish("8080:80"));
    std::string text = report(o, profile());
    CHECK(text.find("access point \"lab\", channel 6, wpa2, internet\n") != std::string::npos);
    CHECK(text.find("pcap     out.pcap\n") != std::string::npos);
    CHECK(text.find("publish  tcp 127.0.0.1:8080 -> guest port 80\n") != std::string::npos);
    // The passphrase is not printed.
    CHECK(text.find("secret123") == std::string::npos);

    o.wifi_ap.reset();
    o.ssid = "home";
    CHECK(report(o, profile()).find("station, joins only \"home\"\n") != std::string::npos);
}

TEST_CASE("config report rejects names the profile lacks") {
    Levels     levels;
    CliOptions o    = options();
    o.uart["UART9"] = "pty";
    CHECK(error_of(o, profile()) == "no UART named UART9 in profile T");

    o = options();
    o.i2c.emplace_back("I2C9", "hi704");
    CHECK(error_of(o, profile()) == "no I2C bus named I2C9 in profile T");

    o = options();
    o.flash_mounts.emplace_back("NOPE", "f.img");
    CHECK(error_of(o, profile()) == "no flash named NOPE in profile T");

    o = options();
    o.loads.push_back({"nowhere", "f.bin"});
    CHECK(error_of(o, profile()) == "unknown load target 'nowhere' (not an alias or a number)");

    Profile none = profile();
    none.modules.erase(none.modules.begin());
    o = options();
    o.flash_mounts.emplace_back("", "f.img");
    CHECK(error_of(o, none) == "no flash in profile T");
}

TEST_CASE("config report of the shipped BK7252 profile") {
    Levels  levels;
    Profile shipped = load_profile("BK7252", BK_PROFILES_DIR);
    // The machine builds from the profile as it ships.
    CliOptions o = options();
    o.i2c.emplace_back("I2C2", "hi704");
    std::string text = report(o, shipped);
    CHECK(text.find("i2c2      0x00802600       0xC     ") != std::string::npos);
    CHECK(text.find("hi704\n") != std::string::npos);
    // The PTA bank is a range of the MAC, not a module of its own.
    CHECK(text.find("pta  0xC0010000  0x28") != std::string::npos);
    CHECK(text.find("MAC_PTA") == std::string::npos);
}
