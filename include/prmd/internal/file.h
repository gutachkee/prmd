#pragma once
// RAII read-only file descriptor with positional reads, shared by the
// decoders.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include "prmd/errors.h"

namespace prmd::internal {

class ReadOnlyFile {
 public:
  // Throws IoError if the file cannot be opened.
  explicit ReadOnlyFile(const std::string& path)
      : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
    if (fd_ < 0) {
      throw IoError("cannot open '" + path + "': " + std::strerror(errno));
    }
  }

  ~ReadOnlyFile() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

  int fd() const { return fd_; }

  uint64_t size() const {
    struct stat st;
    if (::fstat(fd_, &st) != 0) {
      throw IoError("fstat failed");
    }
    return uint64_t(st.st_size);
  }

  // Reads exactly n bytes at `offset` (looping over short reads); returns the
  // number of pread() calls issued. Throws IoError on a read error and
  // FormatError if the file ends early.
  int read_at(uint64_t offset, uint8_t* dst, size_t n) const {
    int calls = 0;
    while (n > 0) {
      const ssize_t r = ::pread(fd_, dst, n, off_t(offset));
      ++calls;
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw IoError(std::string("pread failed: ") + std::strerror(errno));
      }
      if (r == 0) {
        throw FormatError("unexpected end of file");
      }
      dst += r;
      offset += uint64_t(r);
      n -= size_t(r);
    }
    return calls;
  }

 private:
  int fd_;
};

}  // namespace prmd::internal
