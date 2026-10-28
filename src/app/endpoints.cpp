#include "app/endpoints.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <pty.h>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace bk {

namespace {

constexpr size_t kReadChunk = 4096;

void set_flag(int fd, int cmd_get, int cmd_set, int flag) {
    int flags = ::fcntl(fd, cmd_get);
    if (flags >= 0) ::fcntl(fd, cmd_set, flags | flag);
}

// Reads up to kReadChunk bytes from a descriptor that is ready or
// non-blocking; errors and end of file give nothing.
std::vector<uint8_t> read_chunk(int fd) {
    std::vector<uint8_t> buf(kReadChunk);
    ssize_t              n;
    do n = ::read(fd, buf.data(), buf.size());
    while (n < 0 && errno == EINTR);
    buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return buf;
}

// Writes as much as the descriptor takes; the rest is dropped.
void write_some(int fd, std::span<const uint8_t> data) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return; // full (EAGAIN), peer gone (EIO) or any other error
        }
    }
}

class PtyEndpoint : public ByteEndpoint {
  public:
    PtyEndpoint() {
        termios raw{};
        ::cfmakeraw(&raw);
        char name[256] = {};
        if (::openpty(&master_, &slave_, name, &raw, nullptr) != 0)
            throw std::runtime_error(std::string("cannot create a pty: ") + std::strerror(errno));
        path_ = name;
        // openpty may ignore the requested modes on some systems; set them again.
        ::tcsetattr(slave_, TCSANOW, &raw);
        set_flag(master_, F_GETFL, F_SETFL, O_NONBLOCK);
        set_flag(master_, F_GETFD, F_SETFD, FD_CLOEXEC);
        set_flag(slave_, F_GETFD, F_SETFD, FD_CLOEXEC);
    }
    ~PtyEndpoint() override {
        ::close(master_);
        ::close(slave_);
    }

    void                 write(std::span<const uint8_t> data) override { write_some(master_, data); }
    std::vector<uint8_t> read_nowait() override { return read_chunk(master_); }
    std::string          description() const override { return path_; }

  private:
    int         master_ = -1, slave_ = -1;
    std::string path_;
};

// The terminal mode to restore when the process exits without running the
// endpoint's destructor (exit() from anywhere, an escaped exception).
std::mutex                             g_restore_mutex;
std::optional<std::pair<int, termios>> g_restore;

void restore_at_exit() {
    std::lock_guard lock(g_restore_mutex);
    if (g_restore) ::tcsetattr(g_restore->first, TCSADRAIN, &g_restore->second);
    g_restore.reset();
}

class StdioEndpoint : public ByteEndpoint {
  public:
    StdioEndpoint(int in_fd, int out_fd) : in_(in_fd), out_(out_fd) {
        termios saved{};
        if (::isatty(in_) && ::tcgetattr(in_, &saved) == 0) {
            termios raw = saved;
            ::cfmakeraw(&raw);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            // Output keeps the newline translation: the log shares the terminal and ends its lines
            // with a bare \n.
            raw.c_oflag |= OPOST | ONLCR;
            // Ctrl-C stays the way out: it raises SIGINT instead of reaching the firmware. Ctrl-Z
            // and Ctrl-\ are passed to the firmware, so the terminal is never left in raw mode.
            raw.c_lflag |= ISIG;
            raw.c_cc[VSUSP]        = _POSIX_VDISABLE;
            raw.c_cc[VQUIT]        = _POSIX_VDISABLE;
            static bool registered = (std::atexit(restore_at_exit), true);
            (void)registered;
            {
                std::lock_guard lock(g_restore_mutex);
                g_restore = {in_, saved};
            }
            saved_ = saved;
            ::tcsetattr(in_, TCSADRAIN, &raw);
        }
    }
    ~StdioEndpoint() override {
        if (saved_) {
            ::tcsetattr(in_, TCSADRAIN, &*saved_);
            std::lock_guard lock(g_restore_mutex);
            if (g_restore && g_restore->first == in_) g_restore.reset();
        }
    }

    void                 write(std::span<const uint8_t> data) override { write_some(out_, data); }
    std::vector<uint8_t> read_nowait() override {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(in_, &set);
        timeval zero{0, 0};
        if (::select(in_ + 1, &set, nullptr, nullptr, &zero) <= 0) return {};
        return read_chunk(in_);
    }
    std::string description() const override { return "stdio"; }

  private:
    int                    in_, out_;
    std::optional<termios> saved_;
};

// A host serial device at 115200 baud, raw, non-blocking.
class SerialEndpoint : public ByteEndpoint {
  public:
    explicit SerialEndpoint(const std::string &path) : path_(path) {
        fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("cannot open serial device " + path + ": " + std::strerror(errno));
        termios tio{};
        if (::tcgetattr(fd_, &tio) != 0) {
            const std::string why = std::strerror(errno);
            ::close(fd_);
            throw std::runtime_error("cannot use " + path + " as a serial device: " + why);
        }
        ::cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN]  = 0;
        tio.c_cc[VTIME] = 0;
        ::cfsetispeed(&tio, B115200);
        ::cfsetospeed(&tio, B115200);
        if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
            const std::string why = std::strerror(errno);
            ::close(fd_);
            throw std::runtime_error("cannot configure serial device " + path + ": " + why);
        }
    }
    ~SerialEndpoint() override { ::close(fd_); }

    void                 write(std::span<const uint8_t> data) override { write_some(fd_, data); }
    std::vector<uint8_t> read_nowait() override { return read_chunk(fd_); }
    std::string          description() const override { return path_; }

  private:
    int         fd_ = -1;
    std::string path_;
};

} // namespace

std::unique_ptr<ByteEndpoint> make_endpoint(std::string_view spec) {
    if (spec == "none") return nullptr;
    if (spec == "pty") return std::make_unique<PtyEndpoint>();
    if (spec == "stdio") return make_stdio_endpoint(STDIN_FILENO, STDOUT_FILENO);
    return std::make_unique<SerialEndpoint>(std::string(spec));
}

std::unique_ptr<ByteEndpoint> make_stdio_endpoint(int in_fd, int out_fd) {
    return std::make_unique<StdioEndpoint>(in_fd, out_fd);
}

} // namespace bk
