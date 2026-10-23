#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "app/endpoints.h"
#include "periph/uart.h"

using namespace bk;
using namespace std::chrono_literals;

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd &)            = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

// Calls read_nowait() until `want` bytes arrived or one second passed.
std::vector<uint8_t> collect(ByteEndpoint &ep, size_t want) {
    std::vector<uint8_t> got;
    auto                 deadline = SteadyClock::now() + 1s;
    while (got.size() < want && SteadyClock::now() < deadline) {
        auto in = ep.read_nowait();
        got.insert(got.end(), in.begin(), in.end());
        if (in.empty()) ::usleep(1000);
    }
    return got;
}

// Reads from `fd` until `want` bytes arrived or one second passed.
std::vector<uint8_t> collect_fd(int fd, size_t want) {
    std::vector<uint8_t> got;
    auto                 deadline = SteadyClock::now() + 1s;
    while (got.size() < want && SteadyClock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 10) <= 0) continue;
        uint8_t buf[256];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) got.insert(got.end(), buf, buf + n);
    }
    return got;
}

void write_all(int fd, const std::vector<uint8_t> &data) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        REQUIRE(n > 0);
        done += static_cast<size_t>(n);
    }
}

int open_slave(const ByteEndpoint &ep) { return ::open(ep.description().c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK); }

} // namespace

TEST_CASE("pty description is an existing /dev/pts/N") {
    auto        ep   = make_endpoint("pty");
    std::string path = ep->description();
    CHECK(std::regex_match(path, std::regex("/dev/pts/[0-9]+")));
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("pty round trip") {
    auto ep = make_endpoint("pty");
    Fd   slave(open_slave(*ep));
    REQUIRE(slave.fd >= 0);

    write_all(slave.fd, bytes("hello\r\n"));
    CHECK(collect(*ep, 7) == bytes("hello\r\n"));

    auto out = bytes("world\n\x03");
    ep->write(out);
    // Raw mode: no newline translation, no signal characters.
    CHECK(collect_fd(slave.fd, out.size()) == out);
}

TEST_CASE("pty read empty returns empty") {
    auto ep = make_endpoint("pty");
    CHECK(ep->read_nowait().empty());
    Fd slave(open_slave(*ep));
    REQUIRE(slave.fd >= 0);
    CHECK(ep->read_nowait().empty());
}

TEST_CASE("pty read_nowait is bounded at 4096 bytes") {
    auto ep = make_endpoint("pty");
    Fd   slave(open_slave(*ep));
    REQUIRE(slave.fd >= 0);
    std::vector<uint8_t> data(6000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);
    write_all(slave.fd, data);
    ::usleep(50000);

    // The kernel hands a pty master at most about 4 KiB per read anyway; the
    // exact 4096 bound is checked through a pipe in the stdio test below.
    auto first = ep->read_nowait();
    CHECK(first.size() > 0);
    CHECK(first.size() <= 4096);
    CHECK(first.size() < data.size());
    auto rest = collect(*ep, data.size() - first.size());
    first.insert(first.end(), rest.begin(), rest.end());
    CHECK(first == data);
}

TEST_CASE("pty write with closed peer does not block") {
    auto ep = make_endpoint("pty");
    {
        Fd slave(open_slave(*ep));
        REQUIRE(slave.fd >= 0);
    } // the terminal program went away

    // Far more than any pty buffer holds; nobody reads.
    std::vector<uint8_t> chunk(4096, 'x');
    auto                 start = SteadyClock::now();
    for (int i = 0; i < 256; ++i) ep->write(chunk);
    CHECK(SteadyClock::now() - start < 1s);

    // The slave can be reopened and still works.
    Fd again(open_slave(*ep));
    REQUIRE(again.fd >= 0);
    ::tcflush(again.fd, TCIOFLUSH);
    write_all(again.fd, bytes("back"));
    auto in = collect(*ep, 4);
    REQUIRE(in.size() >= 4);
    CHECK(std::vector<uint8_t>(in.end() - 4, in.end()) == bytes("back"));
}

TEST_CASE("uart writes to a pty nobody reads are dropped without blocking") {
    struct ZeroClock : Clock {
        uint64_t now_ns() const override { return 0; }
    } clock;
    auto ep = make_endpoint("pty");
    Uart uart("UART1", clock, Line{});
    uart.write("regs", 0x0C, 4, 'a'); // before any endpoint is attached
    uart.attach(ep.get());
    auto start = SteadyClock::now();
    for (int i = 0; i < 200000; ++i) uart.write("regs", 0x0C, 4, 'a');
    uart.tick(0);
    CHECK(SteadyClock::now() - start < 2s);
}

TEST_CASE("stdio endpoint over non-tty fds does not touch termios") {
    int out[2];
    REQUIRE(::pipe(out) == 0);
    Fd rd(out[0]), wr(out[1]);
    Fd in(::open("/dev/null", O_RDONLY));
    REQUIRE(in.fd >= 0);
    {
        auto ep = make_stdio_endpoint(in.fd, wr.fd);
        CHECK(ep->description() == "stdio");
        CHECK(ep->read_nowait().empty());
        ep->write(bytes("abc"));
    }
    CHECK(collect_fd(rd.fd, 3) == bytes("abc"));
}

TEST_CASE("stdio endpoint puts a terminal in raw mode until destroyed") {
    int master = -1, slave = -1;
    REQUIRE(::openpty(&master, &slave, nullptr, nullptr, nullptr) == 0);
    Fd      m(master), s(slave);
    termios before{};
    REQUIRE(::tcgetattr(s.fd, &before) == 0);
    REQUIRE((before.c_lflag & ICANON) != 0);
    {
        auto    ep = make_stdio_endpoint(s.fd, s.fd);
        termios during{};
        REQUIRE(::tcgetattr(s.fd, &during) == 0);
        CHECK((during.c_lflag & (ICANON | ECHO)) == 0);
        // Ctrl-C still stops the emulator; the other signal characters are off.
        CHECK((during.c_lflag & ISIG) != 0);
        CHECK(during.c_cc[VINTR] == before.c_cc[VINTR]);
        CHECK(during.c_cc[VSUSP] == _POSIX_VDISABLE);
        CHECK(during.c_cc[VQUIT] == _POSIX_VDISABLE);

        write_all(m.fd, bytes("key"));
        CHECK(collect(*ep, 3) == bytes("key"));
    }
    termios after{};
    REQUIRE(::tcgetattr(s.fd, &after) == 0);
    CHECK(after.c_lflag == before.c_lflag);
    CHECK(after.c_iflag == before.c_iflag);
    CHECK(after.c_oflag == before.c_oflag);
}

TEST_CASE("stdio read_nowait is bounded at 4096 bytes") {
    int p[2];
    REQUIRE(::pipe(p) == 0);
    Fd                   rd(p[0]), wr(p[1]);
    Fd                   out(::open("/dev/null", O_WRONLY));
    std::vector<uint8_t> data(5000, 'z');
    write_all(wr.fd, data);
    auto ep = make_stdio_endpoint(rd.fd, out.fd);
    CHECK(ep->read_nowait().size() == 4096);
    CHECK(ep->read_nowait().size() == 904);
    CHECK(ep->read_nowait().empty());
}

TEST_CASE("a serial device endpoint reads and writes through the device") {
    // A pty slave is a terminal like any serial port.
    int  master = -1, slave = -1;
    char name[256] = {};
    REQUIRE(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
    Fd   m(master), s(slave);
    auto ep = make_endpoint(name);
    CHECK(ep->description() == name);
    CHECK(ep->read_nowait().empty());

    write_all(m.fd, bytes("to the emulator"));
    CHECK(collect(*ep, 15) == bytes("to the emulator"));

    ep->write(bytes("from the emulator"));
    std::vector<uint8_t> got;
    const auto           deadline = SteadyClock::now() + 1s;
    while (got.size() < 17 && SteadyClock::now() < deadline) {
        uint8_t buf[64];
        pollfd  p{m.fd, POLLIN, 0};
        if (::poll(&p, 1, 50) > 0) {
            ssize_t n = ::read(m.fd, buf, sizeof buf);
            if (n > 0) got.insert(got.end(), buf, buf + n);
        }
    }
    CHECK(got == bytes("from the emulator"));
}

TEST_CASE("a serial device endpoint puts the port in raw mode at 115200 baud") {
    int  master = -1, slave = -1;
    char name[256] = {};
    REQUIRE(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
    Fd      m(master), s(slave);
    auto    ep = make_endpoint(name);
    termios tio{};
    REQUIRE(::tcgetattr(s.fd, &tio) == 0);
    CHECK((tio.c_lflag & (ICANON | ECHO)) == 0);
    CHECK(::cfgetospeed(&tio) == B115200);
    CHECK(::cfgetispeed(&tio) == B115200);
    // Bytes pass unchanged: no CR/LF translation either way.
    write_all(m.fd, bytes("a\r\nb\n"));
    CHECK(collect(*ep, 5) == bytes("a\r\nb\n"));
}

TEST_CASE("a serial device endpoint swallows write errors") {
    int  master = -1, slave = -1;
    char name[256] = {};
    REQUIRE(::openpty(&master, &slave, name, nullptr, nullptr) == 0);
    auto ep = make_endpoint(name);
    ::close(master); // the peer is gone
    ep->write(bytes("nobody listens"));
    CHECK(ep->read_nowait().empty());
    ::close(slave);
}

TEST_CASE("serial device endpoint errors") {
    auto message = [](const char *spec) -> std::string {
        try {
            make_endpoint(spec);
        } catch (const std::runtime_error &e) {
            return e.what();
        }
        return "(no error)";
    };
    CHECK(message("/dev/bk7252-no-such-serial-device") ==
          "cannot open serial device /dev/bk7252-no-such-serial-device: No such file or directory");
    // Something that exists but is not a terminal.
    CHECK(message("/dev/null").starts_with("cannot use /dev/null as a serial device: "));
    CHECK(message("ptty").starts_with("cannot open serial device ptty: "));
}
