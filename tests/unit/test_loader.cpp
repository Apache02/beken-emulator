#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "app/registry.h"
#include "core/loader.h"
#include "core/machine.h"
#include "core/profile_loader.h"
#include "periph/flash.h"

using namespace bk;
namespace fs = std::filesystem;

namespace {

// A fresh directory under the system temp directory, removed afterwards.
struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path =
            fs::temp_directory_path() / ("bk7252-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path write(const std::string &name, const std::string &text) const {
        fs::path p = path / name;
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
    fs::path write_bytes(const std::string &name, const std::vector<uint8_t> &bytes) const {
        fs::path p = path / name;
        std::ofstream(p, std::ios::binary)
            .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return p;
    }
};

const char *kSmallProfile = R"(
name = "SMALL"
insns_per_us = 50
peripheral_window = [0x00800000, 0x00100000]

[[memory]]
name = "RAM"
base = 0x00400000
size = 0x1000

[alias]
ram = 0x00400000
[alias.boot]
module = "FLASH"
offset = 0

[[module]]
type = "flash"
name = "FLASH"
[module.bases]
xip = 0x00000000
ctrl = 0x00803000
[module.params]
size = 0x1000
)";

// Test chip: two adjacent RAM regions, a 4 KiB flash at 0 and an ICU.
constexpr uint32_t kRam = 0x00400000, kRam2 = 0x00401000, kIcu = 0x00802000;

Profile load_chip() {
    Profile p;
    p.name     = "LOADTEST";
    p.win_base = 0x00800000;
    p.win_size = 0x00100000;
    p.memory   = {
        {"RAM",  kRam,  0x1000, std::nullopt, true},
        {"RAM2", kRam2, 0x1000, std::nullopt, true}
    };
    p.alias["BOOTLOADER"] = {
        FlashAlias{"FLASH", 0}
    };
    p.alias["ODD"] = {
        FlashAlias{"FLASH", 0x10}
    };
    p.alias["BLOCK2"] = {
        FlashAlias{"FLASH", 68}
    };
    p.alias["ICUALIAS"] = {
        FlashAlias{"ICU", 0}
    };
    p.alias["RAM"] = {uint32_t{kRam}};
    p.modules      = {
        {"flash", "FLASH", {{"xip", 0}, {"ctrl", 0x00803000}}, {{"size", 0x1000}}},
        {"intc",  "ICU",   {{"regs", kIcu}},                   {}                }
    };
    return p;
}

std::vector<uint8_t> bytes(size_t n, uint8_t first = 1) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(first + i);
    return v;
}

std::string error_of(Machine &m, const std::vector<LoadSpec> &loads) {
    try {
        plan_loads(m, loads);
    } catch (const LoadError &e) {
        return e.what();
    }
    return "(no error)";
}

} // namespace

// --- profile file ---------------------------------------------------------

TEST_CASE("profile parses minimal bk7252") {
    Profile p = load_profile("BK7252", BK_PROFILES_DIR);
    CHECK(p.name == "BK7252");
    CHECK(p.insns_per_us == 100);
    CHECK(p.win_base == 0x00800000);
    CHECK(p.win_size == 0x00100000);

    REQUIRE(p.memory.size() == 3);
    CHECK(p.memory[0].name == "TCM");
    CHECK(p.memory[0].base == 0x003F8000);
    CHECK(p.memory[0].size == 0x8000);
    CHECK(p.memory[0].reset_fill == std::optional<uint8_t>(0xAA));
    CHECK(p.memory[0].executable);
    CHECK(p.memory[1].name == "RAM1");
    CHECK(p.memory[1].base == 0x00400000);
    CHECK(p.memory[1].size == 0x40000);
    CHECK(p.memory[1].reset_fill == std::optional<uint8_t>(0xAA));
    CHECK_FALSE(p.memory[1].executable);
    CHECK(p.memory[2].name == "RAM2");
    CHECK(p.memory[2].base == 0x00900000);
    CHECK(p.memory[2].executable);

    std::vector<std::string> names, types;
    for (const auto &m : p.modules) {
        names.push_back(m.name);
        types.push_back(m.type);
    }
    CHECK(names == std::vector<std::string>{"FLASH", "ICU", "WDT", "TIMER0", "TIMER1", "UART1", "UART2"});
    CHECK(types == std::vector<std::string>{"flash", "intc", "wdt", "timer", "timer", "uart", "uart"});
    CHECK(p.modules[0].bases == std::map<std::string, uint32_t>{
                                    {"xip",  0         },
                                    {"ctrl", 0x00803000}
    });
    CHECK(p.modules[1].bases.at("regs") == 0x00802000);
    CHECK(p.modules[2].bases.at("regs") == 0x00802900);
    CHECK(p.modules[3].bases.at("regs") == 0x00802A00);
    CHECK(p.modules[4].bases.at("regs") == 0x00802A40);
    CHECK(p.modules[5].bases.at("regs") == 0x00802100);
    CHECK(p.modules[6].bases.at("regs") == 0x00802200);

    const ParamValue *hz = p.modules[4].params.find("hz");
    REQUIRE(hz);
    CHECK(*hz->integer() == 32000);
    const ParamValue *signals = p.modules[1].params.find("signals");
    REQUIRE(signals);
    REQUIRE(signals->table());
    const ParamValue *pwd = signals->table()->find("arm_wdt_pwd");
    REQUIRE(pwd);
    REQUIRE(pwd->list());
    CHECK(*pwd->list() == ParamList{0x08, 8});
    const ParamValue *words = p.modules[1].params.find("reset_words");
    REQUIRE(words);
    REQUIRE(words->table());
    REQUIRE(words->table()->find("0x08"));
    CHECK(*words->table()->find("0x08")->integer() == 0x00ffffff);

    REQUIRE(p.alias.count("RAM"));
    CHECK(std::get<uint32_t>(p.alias["RAM"].target) == 0x00400000);
    CHECK(std::get<uint32_t>(p.alias["IRAM"].target) == 0x00900000);
    REQUIRE(p.alias.count("BOOTLOADER"));
    CHECK(std::get<FlashAlias>(p.alias["BOOTLOADER"].target).module == "FLASH");
    CHECK(std::get<FlashAlias>(p.alias["BOOTLOADER"].target).offset == 0);
    REQUIRE(p.alias.count("APP"));
    CHECK(std::get<FlashAlias>(p.alias["APP"].target).offset == 0x11000);
}

TEST_CASE("shipped profile builds a machine") {
    Profile p = load_profile("BK7252", BK_PROFILES_DIR);
    Machine m(p, builtin_registry());
    for (const char *name : {"FLASH", "ICU", "WDT", "TIMER0", "TIMER1", "UART1", "UART2"})
        CHECK_MESSAGE(m.module(name) != nullptr, name);
    auto regions = m.bus().memory_regions();
    REQUIRE(regions.size() == 3);
    CHECK(regions[0].name == "TCM");
    CHECK(regions[0].base == 0x003F8000);
    CHECK(regions[0].size == 0x8000);
    CHECK(regions[0].executable);
    CHECK(regions[1].name == "RAM1");
    CHECK(regions[1].base == 0x00400000);
    CHECK(regions[1].size == 0x40000);
    CHECK_FALSE(regions[1].executable);
    CHECK(regions[2].name == "RAM2");
    CHECK(regions[2].base == 0x00900000);
    CHECK(regions[2].size == 0x40000);
    CHECK(regions[2].executable);
    // ICU word 0x08 powers the peripheral clocks down at reset.
    CHECK(m.module("ICU")->read("regs", 0x08, 4) == 0x00ffffff);
}

TEST_CASE("profile with a small chip") {
    TempDir dir;
    dir.write("SMALL.toml", kSmallProfile);
    Profile p = load_profile("SMALL", dir.path);
    CHECK(p.name == "SMALL");
    CHECK(p.insns_per_us == 50);
    REQUIRE(p.memory.size() == 1);
    CHECK(p.memory[0].reset_fill == std::nullopt);
    CHECK(p.memory[0].executable);
    // Alias names are stored upper case.
    CHECK(p.alias.count("RAM") == 1);
    CHECK(p.alias.count("BOOT") == 1);
}

TEST_CASE("profile names are restricted to A-Z, 0-9, underscore and dash") {
    TempDir dir;
    dir.write("BK7252.toml", kSmallProfile);
    dir.write("MY_BOARD-2.toml", kSmallProfile);
    const char *msg = "profile name must contain only A-Z, 0-9, '_' and '-': ";
    for (const std::string name : {"../X", "A/B", "/ABS", "A.B", "A\nB", "A B"})
        CHECK_THROWS_WITH_AS(load_profile(name, dir.path), (msg + name).c_str(), ProfileError);
    CHECK_THROWS_WITH_AS(load_profile("", dir.path), "profile name must be upper case", ProfileError);
    CHECK_THROWS_WITH_AS(load_profile("bk7252", dir.path), "profile name must be upper case", ProfileError);
    CHECK_NOTHROW(load_profile("BK7252", dir.path));
    CHECK_NOTHROW(load_profile("MY_BOARD-2", dir.path));
}

TEST_CASE("lower case name rejected") {
    TempDir dir;
    dir.write("SMALL.toml", kSmallProfile);
    CHECK_THROWS_WITH_AS(load_profile("small", dir.path), "profile name must be upper case", ProfileError);
}

TEST_CASE("missing profile lists available ones") {
    TempDir dir;
    dir.write("SMALL.toml", kSmallProfile);
    dir.write("OTHER.toml", kSmallProfile);
    dir.write("notes.txt", "");
    CHECK_THROWS_WITH_AS(load_profile("NOPE", dir.path), "no profile NOPE (available: OTHER, SMALL)", ProfileError);
}

TEST_CASE("bad profile syntax and types rejected") {
    TempDir     dir;
    fs::path    file   = dir.write("BROKEN.toml", "name = \n");
    std::string prefix = file.string() + ": bad profile: ";
    try {
        load_profile("BROKEN", dir.path);
        FAIL("no error");
    } catch (const ProfileError &e) {
        CHECK(std::string(e.what()).starts_with(prefix));
    }

    std::string text = kSmallProfile;
    dir.write("WRONG.toml", text.replace(text.find("size = 0x1000"), 13, "size = \"big\""));
    CHECK_THROWS_WITH_AS(
        load_profile("WRONG", dir.path),
        ((dir.path / "WRONG.toml").string() + ": bad profile: memory RAM: size must be an integer").c_str(),
        ProfileError);

    dir.write("NONAME.toml", "insns_per_us = 1\nperipheral_window = [0, 0]\n");
    CHECK_THROWS_WITH_AS(load_profile("NONAME", dir.path),
                         ((dir.path / "NONAME.toml").string() + ": bad profile: missing key 'name'").c_str(),
                         ProfileError);
}

TEST_CASE("profile alias outside memory rejected") {
    TempDir     dir;
    std::string text = kSmallProfile;
    text.replace(text.find("ram = 0x00400000"), 16, "ram = 0x00500000");
    dir.write("ALIAS.toml", text);
    CHECK_THROWS_WITH_AS(
        load_profile("ALIAS", dir.path),
        ((dir.path / "ALIAS.toml").string() + ": bad profile: alias RAM is outside memory: 0x00500000").c_str(),
        ProfileError);

    text = kSmallProfile;
    text.replace(text.find("module = \"FLASH\""), 16, "module = \"NAND\"");
    dir.write("ALIAS2.toml", text);
    CHECK_THROWS_WITH_AS(
        load_profile("ALIAS2", dir.path),
        ((dir.path / "ALIAS2.toml").string() + ": bad profile: alias BOOT names unknown module NAND").c_str(),
        ProfileError);
}

TEST_CASE("unknown param ignored") {
    TempDir     dir;
    std::string text = kSmallProfile;
    text += "colour = \"blue\"\nfuture = { a = 1 }\nverbose = true\n";
    text.insert(0, "unrelated_top_level_key = 3\n");
    dir.write("SMALL.toml", text);
    Profile p = load_profile("SMALL", dir.path);
    REQUIRE(p.modules.size() == 1);
    const ParamTable &params = p.modules[0].params;
    REQUIRE(params.find("colour"));
    CHECK(*params.find("colour")->string() == "blue");
    REQUIRE(params.find("verbose"));
    CHECK(*params.find("verbose")->integer() == 1);
    Machine m(p, builtin_registry());
    CHECK(m.module("FLASH") != nullptr);
}

TEST_CASE("profile directory next to the executable or from the environment") {
    const char *saved = std::getenv("BK7252_PROFILES");
    std::string old   = saved ? saved : "";
    ::unsetenv("BK7252_PROFILES");
    CHECK(profile_dir("/opt/bk/bin/bk7252-emu") == fs::path("/opt/bk/profiles"));
    ::setenv("BK7252_PROFILES", "/tmp/elsewhere", 1);
    CHECK(profile_dir("/opt/bk/bin/bk7252-emu") == fs::path("/tmp/elsewhere"));
    ::setenv("BK7252_PROFILES", "", 1);
    CHECK(profile_dir("/opt/bk/bin/bk7252-emu") == fs::path("/opt/bk/profiles"));
    if (saved)
        ::setenv("BK7252_PROFILES", old.c_str(), 1);
    else
        ::unsetenv("BK7252_PROFILES");
}

// --- image loading --------------------------------------------------------

TEST_CASE("parse_load splits at first colon") {
    LoadSpec s = parse_load("app:C:\\x");
    CHECK(s.target == "app");
    CHECK(s.file == fs::path("C:\\x"));
    s = parse_load("0x400000:fw.bin");
    CHECK(s.target == "0x400000");
    CHECK(s.file == fs::path("fw.bin"));
    for (const char *bad : {"nocolon", ":file", "target:", ""})
        CHECK_THROWS_WITH_AS(
            parse_load(bad), (std::string("--load expects target:file, got ") + bad).c_str(), LoadError);
}

TEST_CASE("loads go to aliases and ram in command-line order") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a       = dir.write_bytes("a.bin", bytes(8, 0x10));
    fs::path b       = dir.write_bytes("b.bin", bytes(4, 0x80));
    fs::path f       = dir.write_bytes("f.bin", bytes(34, 0x40));
    auto     actions = plan_loads(m,
                                  {
                                  {"ram",                     a},
                                  {"0x400002",                b},
                                  {"bootloader",              f},
                                  {std::to_string(kRam2 + 4), b}
    });
    REQUIRE(actions.size() == 4);
    apply(actions);
    // The second load overwrote bytes 2-5 of the first.
    CHECK(m.cpu().mem_read(kRam, 8) == std::vector<uint8_t>{0x10, 0x11, 0x80, 0x81, 0x82, 0x83, 0x16, 0x17});
    CHECK(m.cpu().mem_read(kRam2 + 4, 4) == std::vector<uint8_t>{0x80, 0x81, 0x82, 0x83});
    auto *flash = dynamic_cast<Flash *>(m.module("FLASH"));
    REQUIRE(flash);
    auto raw = flash->raw();
    CHECK(std::vector<uint8_t>(raw.begin(), raw.begin() + 34) == bytes(34, 0x40));
    CHECK(raw[34] == 0xFF);
}

TEST_CASE("unreadable and unknown targets rejected") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a       = dir.write_bytes("a.bin", bytes(4));
    fs::path missing = dir.path / "missing.bin";
    CHECK(error_of(m,
                   {
                       {"ram", missing}
    })
              .starts_with("cannot read " + missing.string() + ": "));
    CHECK(error_of(m,
                   {
                       {"nowhere", a}
    }) == "unknown load target 'nowhere' (not an alias or a number)");
    CHECK(error_of(m,
                   {
                       {"0x12zz", a}
    }) == "unknown load target '0x12zz' (not an alias or a number)");
    CHECK(error_of(m,
                   {
                       {"-1", a}
    }) == "unknown load target '-1' (not an alias or a number)");
    CHECK(error_of(m,
                   {
                       {"icualias", a}
    }) == "ICU cannot be loaded");
}

TEST_CASE("empty file rejected before any write") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a      = dir.write_bytes("a.bin", bytes(4, 0x10));
    fs::path empty  = dir.write_bytes("empty.bin", {});
    auto     before = m.cpu().mem_read(kRam, 4);
    CHECK(error_of(m,
                   {
                       {"RAM", a    },
                       {"RAM", empty}
    }) == empty.string() + " is empty");
    CHECK(m.cpu().mem_read(kRam, 4) == before);
}

TEST_CASE("alias flash offset not divisible by 34 rejected") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a = dir.write_bytes("a.bin", bytes(4));
    CHECK(error_of(m,
                   {
                       {"odd", a}
    }) ==
          a.string() + ": flash load of 4 bytes at 0x10 must start on a 34-byte block boundary");
    CHECK(plan_loads(m,
                     {
                         {"block2", a}
    })
              .size() == 1);
}

TEST_CASE("flash load larger than flash rejected") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path big = dir.write_bytes("big.bin", bytes(0x1001));
    CHECK(error_of(m,
                   {
                       {"bootloader", big}
    }) ==
          big.string() + ": flash load of 4097 bytes at 0x0 does not fit the 4096 byte flash");
    fs::path whole = dir.write_bytes("whole.bin", bytes(0x1000));
    CHECK(plan_loads(m,
                     {
                         {"bootloader", whole}
    })
              .size() == 1);
}

TEST_CASE("numeric target in module range rejected") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a = dir.write_bytes("a.bin", bytes(4));
    CHECK(error_of(m,
                   {
                       {"0x00802004", a}
    }) ==
          a.string() + ": 0x00802004..0x00802008 lies in ICU.regs, which cannot be loaded");
}

TEST_CASE("numeric target zero refused") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a = dir.write_bytes("a.bin", bytes(4));
    CHECK(error_of(m,
                   {
                       {"0", a}
    }) == a.string() + ": 0x00000000..0x00000004 lies in FLASH.xip, which cannot be loaded");
    CHECK(error_of(m,
                   {
                       {"0x0", a}
    }) ==
          a.string() + ": 0x00000000..0x00000004 lies in FLASH.xip, which cannot be loaded");

    // Refused even where no module claims address 0.
    Profile p = load_chip();
    p.modules.clear();
    p.memory.push_back({"LOW", 0, 0x1000, std::nullopt, true});
    Machine bare(p, builtin_registry());
    CHECK(error_of(bare,
                   {
                       {"0", a}
    }) == a.string() + ": address 0 is the flash window; load flash through an alias");
}

TEST_CASE("ram load must fit region") {
    TempDir  dir;
    Machine  m(load_chip(), builtin_registry());
    fs::path a = dir.write_bytes("a.bin", bytes(8));
    // RAM and RAM2 are adjacent, but a load must fit inside one region.
    CHECK(error_of(m,
                   {
                       {"0x00400ffc", a}
    }) ==
          a.string() + ": 8 bytes at 0x00400FFC does not fit: 0x00400FFC..0x00401004 crosses the end of "
                       "the region RAM at 0x00401000");
    CHECK(error_of(m,
                   {
                       {"0x00500000", a}
    }) == a.string() + ": address 0x00500000 is outside memory");
    CHECK(error_of(m,
                   {
                       {"0x1ffffffff", a}
    }) == a.string() + ": address 0x1FFFFFFFF is outside memory");
    CHECK(plan_loads(m,
                     {
                         {"0x00400ff8", a}
    })
              .size() == 1);
}

TEST_CASE("a file beyond every load target is rejected without reading it all") {
    TempDir dir;
    Machine m(load_chip(), builtin_registry());
    // The largest target of the test chip is 0x1000 bytes.
    fs::path big = dir.path / "big.bin";
    std::ofstream(big, std::ios::binary) << 'x';
    std::error_code ec;
    fs::resize_file(big, (uint64_t{1} << 32) + 16, ec);
    if (ec) return; // the filesystem refuses sparse files
    for (const char *target : {"ram", "bootloader"})
        CHECK(error_of(m,
                       {
                           {target, big}
        })
                  .find(big.string() + ": file is too large") == 0);
    // An endless character device stops at the same cap.
    if (fs::exists("/dev/zero"))
        CHECK(error_of(m,
                       {
                           {"ram", "/dev/zero"}
        })
                  .find("/dev/zero: file is too large") == 0);
    // A file exactly at the cap is still read and checked normally.
    fs::path exact = dir.write_bytes("exact.bin", std::vector<uint8_t>(0x1000, 1));
    CHECK(error_of(m,
                   {
                       {"bootloader", exact}
    }) == "(no error)");
}
