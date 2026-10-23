#include <doctest/doctest.h>

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <pty.h>
#include <string>
#include <sys/ioctl.h>
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

    explicit Child(std::vector<std::string> args, bool stdio_on_tty = false) {
        REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
        REQUIRE(::tcgetattr(slave, &initial) == 0);
        int e[2];
        REQUIRE(::pipe(e) == 0);
        args.insert(args.begin(), BK7252_EXE);
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
            ::setenv("BK7252_PROFILES", BK_PROFILES_DIR, 1);
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
