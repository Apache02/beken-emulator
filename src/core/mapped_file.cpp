#include "core/mapped_file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace bk {

MappedFile::MappedFile(std::string path, size_t size) : path_(std::move(path)), size_(size) {
    auto fail = [this](const std::string &what) {
        std::string message = path_ + ": " + what;
        if (errno) message += std::string(": ") + std::strerror(errno);
        if (fd_ >= 0) ::close(fd_);
        // A file this open created is not left behind half made.
        if (created_) ::unlink(path_.c_str());
        throw std::runtime_error(message);
    };
    if (size_ == 0) throw std::invalid_argument(path_ + ": a mapped file must not be empty");
    errno = 0;
    fd_   = ::open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd_ >= 0) {
        created_ = true;
    } else if (errno == EEXIST) {
        fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    }
    if (fd_ < 0) fail("cannot open");
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            errno = 0;
            fail("is already in use");
        }
        fail("cannot lock");
    }
    if (created_) {
        if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) fail("cannot set the size");
    } else {
        struct stat st {};
        if (::fstat(fd_, &st) != 0) fail("cannot read the size");
        if (static_cast<uint64_t>(st.st_size) != size_) {
            errno = 0;
            fail("is " + std::to_string(st.st_size) + " bytes, the flash is " + std::to_string(size_));
        }
    }
    void *data = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data == MAP_FAILED) fail("cannot map");
    data_ = static_cast<uint8_t *>(data);
}

MappedFile::~MappedFile() {
    ::munmap(data_, size_);
    ::close(fd_);
}

} // namespace bk
