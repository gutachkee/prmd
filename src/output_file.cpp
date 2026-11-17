// Replace-on-success output file (include/prmd/internal/output_file.h).

#include "prmd/internal/output_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>

#include "prmd/errors.h"

namespace prmd::internal {

namespace {

constexpr int kMaxSymlinks = 40;  // as the kernel's ELOOP limit

std::string dir_of(const std::string& p) {
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) {
    return "";
  }
  return p.substr(0, slash + 1);  // keeps the '/' ("/" for the root)
}

// `p` with symbolic links in its last component followed into `*out` (a
// dangling link gives the path it points to, which fopen would create).
// Directories in the path are left to the kernel. False after more than
// kMaxSymlinks links, or if a link cannot be read: fopen would fail
// (ELOOP), and the path left would be a link, which rename() would replace.
bool follow_links(const std::string& p, std::string* out) {
  std::string cur = p;
  for (int hop = 0;; ++hop) {
    struct stat st;
    if (::lstat(cur.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) {
      *out = cur;
      return true;
    }
    if (hop == kMaxSymlinks) {
      return false;
    }
    std::string buf(size_t(st.st_size > 0 ? st.st_size : 255) + 1, '\0');
    const ssize_t n = ::readlink(cur.c_str(), &buf[0], buf.size());
    if (n < 0 || size_t(n) >= buf.size()) {
      return false;  // changed under us
    }
    buf.resize(size_t(n));
    cur = (!buf.empty() && buf[0] == '/') ? buf : dir_of(cur) + buf;
  }
}

// "<dir>/<name>.tmp.<pid>." for the temporary files of `target`, the name
// cut short so that the full temporary name (16 hex digits follow) fits in
// the directory's NAME_MAX.
std::string temp_prefix(const std::string& target) {
  const std::string dir = dir_of(target);
  std::string name = target.substr(dir.size());
  const std::string tail = ".tmp." + std::to_string(::getpid()) + ".";
  long name_max = ::pathconf(dir.empty() ? "." : dir.c_str(), _PC_NAME_MAX);
  if (name_max <= 0) {
    name_max = 255;  // NAME_MAX on Linux
  }
  const size_t fixed = tail.size() + 16;
  const size_t room = size_t(name_max) > fixed ? size_t(name_max) - fixed : 0;
  if (name.size() > room) {
    name.resize(room);
  }
  return dir + name + tail;
}

// Whether rename() over the existing `target` (status `st`) would be
// refused although the caller may write it: in a sticky directory (/tmp)
// only the owner of the file or of the directory may replace an entry.
bool sticky_refuses_rename(const std::string& target, const struct stat& st) {
  const uid_t me = ::geteuid();
  if (me == 0 || st.st_uid == me) {
    return false;  // root has CAP_FOWNER; our own file may be replaced
  }
  const std::string dir = dir_of(target);
  struct stat ds;
  if (::stat(dir.empty() ? "." : dir.c_str(), &ds) != 0) {
    return false;  // let the temporary file's creation decide
  }
  return (ds.st_mode & S_ISVTX) != 0 && ds.st_uid != me;
}

uint64_t random_suffix() {
  static thread_local std::mt19937_64 rng([] {
    uint64_t seed = uint64_t(
        std::chrono::steady_clock::now().time_since_epoch().count());
    try {
      std::random_device rd;
      seed ^= (uint64_t(rd()) << 32) ^ rd();
    } catch (...) {
      // No entropy source: the clock, the pid and O_EXCL suffice.
    }
    return seed ^ (uint64_t(::getpid()) << 16);
  }());
  return rng();
}

}  // namespace

void check_output_path(const std::string& path) {
  if (path.empty()) {
    throw IoError("cannot create ''");
  }
}

OutputFile::OutputFile(const std::string& path) : path_(path) {
  const std::string cannot = "cannot create '" + path_ + "'";
  check_output_path(path_);
  if (!follow_links(path_, &target_)) {
    throw IoError(cannot);  // ELOOP, as fopen
  }
  struct stat st;
  const bool exists = ::stat(target_.c_str(), &st) == 0;
  if (exists && !S_ISREG(st.st_mode)) {
    // Not replaceable (directory, device, FIFO): what fopen "wb" does.
    direct_ = true;
    write_path_ = target_;
    fd_ = ::open(target_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                 0666);
    if (fd_ < 0) {
      throw IoError(cannot);
    }
    return;
  }
  if (exists && ::faccessat(AT_FDCWD, target_.c_str(), W_OK, AT_EACCESS) != 0) {
    throw IoError(cannot);  // fopen would be refused
  }
  // A writable regular file that cannot be replaced: written in place,
  // truncated first, as fopen "wb" does (no O_CREAT: it exists, and in a
  // sticky directory the kernel may refuse O_CREAT on another user's file).
  const auto open_direct = [&] {
    direct_ = true;
    write_path_ = target_;
    fd_ = ::open(target_.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd_ < 0) {
      throw IoError(cannot);
    }
  };
  if (exists && sticky_refuses_rename(target_, st)) {
    open_direct();
    return;
  }

  // Name: the target's (shortened if needed), then ".tmp.<pid>.<random>"
  // (16 hex digits).
  const std::string prefix = temp_prefix(target_);
  for (int attempt = 0; attempt < 100 && fd_ < 0; ++attempt) {
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx",
                  static_cast<unsigned long long>(random_suffix()));
    write_path_ = prefix + hex;
    fd_ = ::open(write_path_.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd_ < 0 && errno != EEXIST) {
      break;
    }
  }
  if (fd_ < 0) {
    // No temporary file next to the target (read-only directory, name or
    // path too long, ...). An existing writable file is still written as
    // fopen "wb" would, without the replace-on-success guarantee.
    write_path_.clear();
    if (exists) {
      open_direct();
      return;
    }
    throw IoError(cannot);
  }
  if (exists && ::fchmod(fd_, st.st_mode & 0777) != 0) {
    // Keep the umask-based mode; the contents matter more.
  }
}

OutputFile::~OutputFile() {
  if (!committed_) {
    close_quietly();
    if (!direct_ && !write_path_.empty()) {
      ::unlink(write_path_.c_str());
    }
  }
}

FILE* OutputFile::stream() {
  if (!stream_ && fd_ >= 0) {
    stream_ = ::fdopen(fd_, "wb");
    if (!stream_) {
      throw IoError("write failed for '" + path_ + "'");
    }
    std::setvbuf(stream_, nullptr, _IOFBF, size_t(1) << 20);
  }
  return stream_;
}

void OutputFile::close_quietly() {
  if (stream_) {
    std::fclose(stream_);  // also closes fd_
  } else if (fd_ >= 0) {
    ::close(fd_);
  }
  stream_ = nullptr;
  fd_ = -1;
}

unsigned long long OutputFile::commit() {
  const std::string failed = "write failed for '" + path_ + "'";
  bool ok = !failed_;
  if (stream_) {
    ok = std::fflush(stream_) == 0 && !std::ferror(stream_) && ok;
  }
  struct stat st;
  st.st_size = 0;
  ok = ok && ::fstat(fd_, &st) == 0;
  bool closed;
  if (stream_) {
    closed = std::fclose(stream_) == 0;
  } else {
    closed = ::close(fd_) == 0;
  }
  stream_ = nullptr;
  fd_ = -1;
  if (ok && closed &&
      (direct_ || ::rename(write_path_.c_str(), target_.c_str()) == 0)) {
    committed_ = true;
    return static_cast<unsigned long long>(st.st_size);
  }
  if (!direct_) {
    ::unlink(write_path_.c_str());
  }
  committed_ = true;  // nothing left for the destructor
  throw IoError(failed);
}

}  // namespace prmd::internal
