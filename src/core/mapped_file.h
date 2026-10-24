#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace bk {

// A file of a fixed size mapped into memory and shared with it: every store to
// bytes() reaches the file, written back by the kernel, so it is kept even if
// the process dies. While open, the file is locked (flock) against a second
// user, this process included.
class MappedFile {
  public:
    // Opens `path`, creating it with `size` zero bytes when it does not exist.
    // Throws std::runtime_error when the file cannot be opened, locked or
    // mapped, or when an existing file is not `size` bytes long. `size` must
    // not be 0.
    MappedFile(std::string path, size_t size);
    ~MappedFile();
    MappedFile(const MappedFile &)            = delete;
    MappedFile &operator=(const MappedFile &) = delete;

    std::span<uint8_t> bytes() const { return {data_, size_}; }
    const std::string &path() const { return path_; }
    // Whether this open created the file.
    bool created() const { return created_; }

  private:
    std::string path_;
    size_t      size_;
    int         fd_      = -1;
    uint8_t    *data_    = nullptr;
    bool        created_ = false;
};

} // namespace bk
