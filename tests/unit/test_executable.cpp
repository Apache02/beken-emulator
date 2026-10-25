#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "periph/crc16.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

using SteadyClock = std::chrono::steady_clock;

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path               = fs::temp_directory_path() /
               ("bk7252-exe-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A flash image of one 34-byte block holding the given ARM words, with a
// valid CRC.
fs::path flash_image(const TempDir &dir, const std::string &name, const std::vector<uint32_t> &words) {
    std::vector<uint8_t> block(34, 0xFF);
    for (size_t i = 0; i < words.size(); ++i)
        for (size_t b = 0; b < 4; ++b) block[4 * i + b] = static_cast<uint8_t>(words[i] >> (8 * b));
    uint16_t crc = bk::crc16(std::span<const uint8_t>(block.data(), 32));
    block[32]    = static_cast<uint8_t>(crc >> 8);
    block[33]    = static_cast<uint8_t>(crc);
    fs::path p   = dir.path / name;
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char *>(block.data()), static_cast<std::streamsize>(block.size()));
    return p;
}

constexpr uint32_t kBranchSelf  = 0xEAFFFFFE; // b .
constexpr uint32_t kWfi         = 0xEE070F90; // mcr p15, 0, r0, c7, c0, 4
constexpr uint32_t kBranchBack4 = 0xEAFFFFFD; // b . - 4

// A running emulator with stdout (and optionally stdin) on a pty, stderr on a
// pipe.
struct Child {
    pid_t       pid    = -1;
    int         master = -1, slave = -1, err = -1;
    std::string out_text, err_text;
    termios     initial{}; // the pty's modes before the emulator started

    explicit Child(std::vector<std::string> args, bool stdio_on_tty = false, std::string profiles = BK_PROFILES_DIR) {
        REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        REQUIRE(::tcgetattr(slave, &initial) == 0);
        int e[2];
        REQUIRE(::pipe(e) == 0);
        args.insert(args.begin(), BKEMU_EXE);
        pid = ::fork();
        REQUIRE(pid >= 0);
        if (pid == 0) {
            ::setsid();
            if (stdio_on_tty) ::ioctl(slave, TIOCSCTTY, 0); // the terminal becomes the controlling one
            ::dup2(stdio_on_tty ? slave : ::open("/dev/null", O_RDONLY), 0);
            ::dup2(slave, 1);
            ::dup2(e[1], 2);
            std::vector<char *> argv;
            for (auto &a : args) argv.push_back(a.data());
            argv.push_back(nullptr);
            ::setenv("BKEMU_PROFILES", profiles.c_str(), 1);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        ::close(e[1]);
        err = e[0];
    }
    Child(const Child &)            = delete;
    Child &operator=(const Child &) = delete;
    ~Child() {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        ::close(master);
        ::close(slave);
        ::close(err);
    }

    void drain(int timeout_ms) {
        pollfd p[2] = {
            {master, POLLIN, 0},
            {err,    POLLIN, 0}
        };
        if (::poll(p, 2, timeout_ms) <= 0) return;
        char buf[1024];
        if (p[0].revents & POLLIN) {
            ssize_t n = ::read(master, buf, sizeof buf);
            if (n > 0) out_text.append(buf, static_cast<size_t>(n));
        }
        if (p[1].revents & POLLIN) {
            ssize_t n = ::read(err, buf, sizeof buf);
            if (n > 0) err_text.append(buf, static_cast<size_t>(n));
        }
    }

    // Waits up to `limit` for stdout to contain `text`.
    bool wait_for_output(const std::string &text, std::chrono::milliseconds limit) {
        auto deadline = SteadyClock::now() + limit;
        while (out_text.find(text) == std::string::npos && SteadyClock::now() < deadline) drain(10);
        return out_text.find(text) != std::string::npos;
    }

    // Exit status, or nullopt if the process is still running after `limit`.
    std::optional<int> wait_exit(std::chrono::milliseconds limit) {
        auto deadline = SteadyClock::now() + limit;
        while (true) {
            int   status = 0;
            pid_t r      = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                pid = -1;
                for (int i = 0; i < 5; ++i) drain(1);
                if (WIFEXITED(status)) return WEXITSTATUS(status);
                return 128 + WTERMSIG(status);
            }
            if (SteadyClock::now() >= deadline) return std::nullopt;
            drain(5);
        }
    }
};

void stops_cleanly(const std::vector<uint32_t> &program, int sig) {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", program);
    Child    c({"BK7252", "--load", "bootloader:" + image.string()});
    REQUIRE(c.wait_for_output("UART2: ", 2000ms));
    CHECK(c.out_text.find("UART1: /dev/pts/") != std::string::npos);
    ::usleep(200000); // let it run
    auto sent = SteadyClock::now();
    REQUIRE(::kill(c.pid, sig) == 0);
    auto status = c.wait_exit(1000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 0);
    CHECK(SteadyClock::now() - sent < 1s);
    CHECK(c.err_text.find("error") == std::string::npos);
}

} // namespace

TEST_CASE("executable --help prints usage and exits 0") {
    for (const char *flag : {"--help", "-h"}) {
        Child c({flag});
        auto  status = c.wait_exit(2000ms);
        REQUIRE(status.has_value());
        CHECK(*status == 0);
        c.drain(100);
        CHECK(c.out_text.starts_with("Usage: bkemu PROFILE --load TARGET:FILE"));
        CHECK(c.err_text.empty());
    }
}

TEST_CASE("executable without --load exits 2") {
    Child c({"BK7252"});
    auto  status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(c.err_text == "error: at least one --load is required\n");
}

TEST_CASE("executable with a missing load file exits 2") {
    Child c({"BK7252", "--load", "bootloader:/nonexistent/image.bin"});
    auto  status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(c.err_text.starts_with("error: cannot read /nonexistent/image.bin"));
}

TEST_CASE("executable with an unknown profile exits 2") {
    Child c({"NOPE", "--load", "bootloader:/nonexistent/image.bin"});
    auto  status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(c.err_text.starts_with("error: no profile NOPE"));
}

TEST_CASE("executable with a serial device it cannot open exits 2") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    Child    c({"BK7252", "--load", "bootloader:" + image.string(), "--uart2", "/dev/bk7252-no-such-serial-device"});
    auto     status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(c.err_text ==
          "error: cannot open serial device /dev/bk7252-no-such-serial-device: No such file or directory\n");
}

TEST_CASE("executable refuses a --uart name the profile does not have") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    Child    c({"BK7252", "--load", "bootloader:" + image.string(), "--uart", "UART9=stdio"});
    auto     status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(c.err_text == "error: no UART named UART9 in profile BK7252\n");
    // TIMER0 exists but is not a UART.
    Child t({"BK7252", "--load", "bootloader:" + image.string(), "--uart", "TIMER0=stdio"});
    status = t.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(t.err_text == "error: no UART named TIMER0 in profile BK7252\n");
}

TEST_CASE("executable keeps the flash in a mounted file") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    fs::path disk  = dir.path / "disk.bin";
    auto     run   = [&] {
        auto c = std::make_unique<Child>(std::vector<std::string>{
            "BK7252", "--flash-mount", disk.string(), "--load", "bootloader:" + image.string()});
        REQUIRE(c->wait_for_output("UART2: ", 2000ms));
        ::kill(c->pid, SIGKILL); // the file must not depend on a clean exit
        REQUIRE(c->wait_exit(1000ms).has_value());
        return c;
    };
    auto first = run();
    CHECK(first->err_text.find("FLASH: created " + disk.string()) != std::string::npos);
    std::ifstream        in(disk, std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() == 0x200000);
    std::ifstream        prog(image, std::ios::binary);
    std::vector<uint8_t> block{std::istreambuf_iterator<char>(prog), std::istreambuf_iterator<char>()};
    CHECK(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 34) == block);
    CHECK(std::all_of(bytes.begin() + 34, bytes.end(), [](uint8_t b) { return b == 0xFF; }));

    auto second = run();
    CHECK(second->err_text.find("FLASH: mounted " + disk.string()) != std::string::npos);
    CHECK(second->err_text.find("leaves " + disk.string() + " unchanged") != std::string::npos);
}

TEST_CASE("executable reports bad flash mounts with exit 2") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    auto     error = [&](std::vector<std::string> mount) {
        std::vector<std::string> args{"BK7252", "--load", "bootloader:" + image.string()};
        args.insert(args.end(), mount.begin(), mount.end());
        Child c(args);
        auto  status = c.wait_exit(2000ms);
        REQUIRE(status.has_value());
        CHECK(*status == 2);
        return c.err_text;
    };
    CHECK(error({"--flash-mount", "UART1=" + (dir.path / "a.bin").string()}) ==
          "error: no flash named UART1 in profile BK7252\n");
    CHECK(error({"--flash-mount", (dir.path / "a.bin").string(), "--flash-mount", (dir.path / "b.bin").string()})
              .ends_with("error: --flash-mount given twice for FLASH\n"));
    std::ofstream(dir.path / "short.bin") << "x";
    CHECK(error({"--flash-mount", (dir.path / "short.bin").string()}) ==
          "error: " + (dir.path / "short.bin").string() + ": is 1 bytes, the flash is 2097152\n");
}

TEST_CASE("executable reports bad I2C requests with exit 2") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    auto     error = [&](std::vector<std::string> extra) {
        std::vector<std::string> args{"BK7252", "--load", "bootloader:" + image.string()};
        args.insert(args.end(), extra.begin(), extra.end());
        Child c(args);
        auto  status = c.wait_exit(2000ms);
        REQUIRE(status.has_value());
        CHECK(*status == 2);
        return c.err_text;
    };
    CHECK(error({"--i2c1", "ov7670"}) == "error: unknown I2C device 'ov7670' (known: hi704)\n");
    CHECK(error({"--i2c1", "hi704@0x99"}) ==
          "error: bad I2C device 'hi704@0x99' (want TYPE[@ADDRESS], ADDRESS a 7-bit number 0x08-0x77)\n");
    CHECK(error({"--i2c", "I2C9=hi704"}) == "error: no I2C bus named I2C9 in profile BK7252\n");
    CHECK(error({"--i2c", "UART1=hi704"}) == "error: no I2C bus named UART1 in profile BK7252\n");
    CHECK(error({"--i2c1", "hi704", "--i2c1", "hi704"}) ==
          "error: I2C1 address 0x30 is used twice (hi704 and hi704)\n");
}

TEST_CASE("executable reports bad publish requests with exit 2") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    auto     error = [&](std::vector<std::string> extra, const std::string &profile = "BK7252") {
        std::vector<std::string> args{profile, "--load", "bootloader:" + image.string()};
        args.insert(args.end(), extra.begin(), extra.end());
        Child c(args);
        auto  status = c.wait_exit(2000ms);
        REQUIRE(status.has_value());
        CHECK(*status == 2);
        return c.err_text;
    };
    CHECK(error({"-p", "http"}) ==
          "error: bad --publish 'http' (want [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp], ports 1-65535)\n");
    CHECK(error({"--ssid", ""}) == "error: --ssid needs a name of 1 to 32 bytes\n");

    // A port somebody else listens on.
    int taken = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(taken >= 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(taken, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0);
    REQUIRE(::listen(taken, 1) == 0);
    socklen_t len = sizeof sa;
    REQUIRE(::getsockname(taken, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
    std::string port = std::to_string(ntohs(sa.sin_port));
    CHECK(error({"-p", port + ":80"}) ==
          "error: cannot publish 127.0.0.1:" + port + " to guest port 80/tcp: Address already in use\n");
    ::close(taken);
}

TEST_CASE("executable publish needs a MAC module") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    // A profile without a WiFi MAC.
    fs::path profiles = dir.path / "profiles";
    fs::create_directories(profiles);
    std::ofstream(profiles / "NOMAC.toml") << "name = \"NOMAC\"\ninsns_per_us = 100\n"
                                              "peripheral_window = [0x00800000, 0x00100000]\n"
                                              "[[memory]]\nname = \"RAM\"\nbase = 0x00400000\nsize = 0x1000\n"
                                              "[alias]\nRAM = 0x00400000\n";
    auto error = [&](const char *flag, const char *value) {
        Child c({"NOMAC", "--load", "RAM:" + image.string(), flag, value}, false, profiles.string());
        auto  status = c.wait_exit(2000ms);
        REQUIRE(status.has_value());
        CHECK(*status == 2);
        return c.err_text;
    };
    CHECK(error("--publish", "8080:80") == "error: --publish needs a profile with a WiFi MAC module named MAC\n");
    CHECK(error("--ssid", "cam") == "error: --ssid needs a profile with a WiFi MAC module named MAC\n");
}

TEST_CASE("executable --wifi-ap needs a MAC module") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    // A profile without a WiFi MAC.
    fs::path profiles = dir.path / "profiles";
    fs::create_directories(profiles);
    std::ofstream(profiles / "NOMAC.toml") << "name = \"NOMAC\"\ninsns_per_us = 100\n"
                                              "peripheral_window = [0x00800000, 0x00100000]\n"
                                              "[[memory]]\nname = \"RAM\"\nbase = 0x00400000\nsize = 0x1000\n"
                                              "[alias]\nRAM = 0x00400000\n";
    Child c({"NOMAC", "--load", "RAM:" + image.string(), "--wifi-ap", "TESTNET", "-p", "8080:80"},
            false,
            profiles.string());
    auto  status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 2);
    CHECK(c.err_text == "error: --wifi-ap needs a profile with a WiFi MAC module named MAC\n");
}

TEST_CASE("executable publishes a port, prints it and stops on SIGTERM") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kBranchSelf});
    Child    c({"BK7252",
                "--load",
                "bootloader:" + image.string(),
                "-p",
                "127.0.0.1:40111:80",
                "-p",
                "127.0.0.1:40112:53/udp"});
    // One line per publish, after the network is created and before the UARTs.
    bool listed = c.wait_for_output("udp 127.0.0.1:40112 -> guest port 53", 5000ms);
    // If a port is taken on this machine, the emulator says so here.
    INFO(c.err_text);
    CHECK(listed);
    CHECK(c.out_text.find("tcp 127.0.0.1:40111 -> guest port 80") != std::string::npos);
    CHECK(c.out_text.find("tcp 127.0.0.1:40111") < c.out_text.find("udp 127.0.0.1:40112"));
    ::kill(c.pid, SIGTERM);
    auto status = c.wait_exit(3000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 0);
}

TEST_CASE("executable stops on SIGINT while running") { stops_cleanly({kBranchSelf}, SIGINT); }

TEST_CASE("executable stops on SIGTERM while running") { stops_cleanly({kBranchSelf}, SIGTERM); }

TEST_CASE("executable stops on SIGINT while idle") { stops_cleanly({kWfi, kBranchBack4}, SIGINT); }

TEST_CASE("executable stops on SIGTERM while idle") { stops_cleanly({kWfi, kBranchBack4}, SIGTERM); }

TEST_CASE("executable stops on Ctrl-C typed on a stdio terminal") {
    TempDir  dir;
    fs::path image = flash_image(dir, "prog.bin", {kWfi, kBranchBack4});
    Child    c({"BK7252", "--load", "bootloader:" + image.string(), "--uart2", "stdio"}, true);
    REQUIRE(c.wait_for_output("UART1: ", 2000ms));
    ::usleep(200000);
    const char etx = 0x03;
    REQUIRE(::write(c.master, &etx, 1) == 1);
    auto status = c.wait_exit(2000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 0);
}

TEST_CASE("executable restores a stdio terminal on SIGTERM") {
    TempDir        dir;
    fs::path       image = flash_image(dir, "prog.bin", {kWfi, kBranchBack4});
    Child          c({"BK7252", "--load", "bootloader:" + image.string(), "--uart1", "stdio"}, true);
    const termios &before = c.initial;
    REQUIRE((before.c_lflag & ICANON) != 0);
    REQUIRE(c.wait_for_output("UART2: ", 2000ms));
    CHECK(c.out_text.find("UART1: stdio") != std::string::npos);
    termios during{};
    REQUIRE(::tcgetattr(c.slave, &during) == 0);
    CHECK((during.c_lflag & ICANON) == 0);

    REQUIRE(::kill(c.pid, SIGTERM) == 0);
    auto status = c.wait_exit(1000ms);
    REQUIRE(status.has_value());
    CHECK(*status == 0);
    termios after{};
    REQUIRE(::tcgetattr(c.slave, &after) == 0);
    CHECK(after.c_lflag == before.c_lflag);
    CHECK(after.c_iflag == before.c_iflag);
    CHECK(after.c_oflag == before.c_oflag);
}
