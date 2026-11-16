#pragma once
// Replace-on-success output file for the encoders (design.md §1.4,
// "Errors").
//
// The bytes go to a new temporary file in the target's directory, named
// "<name>.tmp.<pid>.<random>" and created exclusively (O_EXCL), with the
// permissions a plain fopen(path, "wb") would give: 0666 minus the umask
// for a new file, the existing file's permission bits when one is replaced.
// commit() closes it, checking every error, and rename()s it over the
// target. If anything fails before that, or commit() is never called, the
// temporary file is removed and an existing target is left exactly as it
// was: never truncated, never partially written.
//
// Target kinds:
//   * a symbolic link is followed (as fopen would): the file it points to is
//     replaced, the link itself stays;
//   * an existing regular file that the caller may not write is not
//     replaced (IoError "cannot create", as fopen would fail), although
//     rename() alone could do it in a writable directory;
//   * anything else that exists and is not a regular file (a device such as
//     /dev/null, a FIFO) is opened and written directly, without a
//     temporary file: it cannot be replaced, and a partial write to it is
//     what the device received.
// Replacing gives the target a new inode: hard links to the old file keep
// the old contents, and its owner and group become the writer's (permission
// bits are kept). There is no fsync, as before (fopen + fclose): the rename
// is atomic for readers and for failures of the write itself, not for a
// system crash right after it.

#include <cstddef>
#include <cstdio>
#include <string>

namespace prmd::internal {

class OutputFile {
 public:
  // Creates the temporary file (or opens a non-regular target). Throws
  // IoError("cannot create '<path>'") when it cannot.
  explicit OutputFile(const std::string& path);
  ~OutputFile();  // removes the temporary file unless committed
  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;

  // The file actually being written (the temporary file, or the target for
  // a non-regular one).
  const std::string& write_path() const { return write_path_; }

  // Buffered stdio stream on the file, owned by this object.
  FILE* stream();
  // Raw descriptor of the file (do not mix with stream()).
  int fd() const { return fd_; }

  // Records a failed write; commit() then throws instead of renaming.
  void fail() { failed_ = true; }

  // Flushes and closes the file and renames it over the target. Throws
  // IoError("write failed for '<path>'") on any error (the temporary file
  // is removed, an existing target is untouched). Returns the file size.
  unsigned long long commit();

 private:
  void close_quietly();

  std::string path_;        // as given, for messages
  std::string target_;      // symlinks resolved
  std::string write_path_;  // temporary file, or target_ when direct
  bool direct_ = false;     // writing the (non-regular) target itself
  int fd_ = -1;
  FILE* stream_ = nullptr;
  bool failed_ = false;
  bool committed_ = false;
};

}  // namespace prmd::internal
