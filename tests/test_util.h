#pragma once
// Shared helpers for codec tests.

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <string>
#include <vector>

#include "prmd/encoder.h"
#include "prmd/image.h"

namespace prmd {

inline void PrintTo(ColorFormat f, std::ostream* os) {
  *os << info(f).name;
}

}  // namespace prmd

namespace testutil {

// remove_all, also of a tree in which a failed test left a directory
// read-only (tests/test_output_file.cpp): its owner bits are restored first.
inline void force_remove_all(const std::filesystem::path& p) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::remove_all(p, ec);
  if (!ec) {
    return;
  }
  for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code pec;
    if (it->is_directory(pec) && !it->is_symlink(pec)) {
      fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add,
                      pec);
    }
  }
  fs::remove_all(p, ec);
}

// Removes the prmd_tests_<pid> directories that test processes killed
// before their exit (a ctest timeout, a crash) left in `tmp`: only ours,
// untouched for an hour, and whose process no longer exists.
inline void remove_stale_test_dirs(const std::filesystem::path& tmp) {
  namespace fs = std::filesystem;
  const std::string prefix = "prmd_tests_";
  const auto now = fs::file_time_type::clock::now();
  std::error_code ec;
  for (fs::directory_iterator it(tmp, ec), end; !ec && it != end;
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind(prefix, 0) != 0 || name.size() == prefix.size() ||
        name.find_first_not_of("0123456789", prefix.size()) !=
            std::string::npos) {
      continue;
    }
    struct stat st;
    if (::lstat(it->path().c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != ::geteuid()) {
      continue;
    }
    std::error_code tec;
    const auto mtime = fs::last_write_time(it->path(), tec);
    if (tec || now - mtime < std::chrono::hours(1)) {
      continue;
    }
    const long pid = std::strtol(name.c_str() + prefix.size(), nullptr, 10);
    if (pid <= 0 || pid == long(::getpid()) ||
        !(::kill(pid_t(pid), 0) != 0 && errno == ESRCH)) {
      continue;
    }
    force_remove_all(it->path());
  }
}

inline std::filesystem::path temp_path(const std::string& name) {
  // Per-process directory: ctest runs every test as its own process, in
  // parallel. Removed at exit; directories left by killed processes are
  // removed by a later run.
  struct Dir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("prmd_tests_" + std::to_string(::getpid()));

    Dir() {
      remove_stale_test_dirs(path.parent_path());
      std::filesystem::create_directories(path);
    }

    ~Dir() { force_remove_all(path); }  // clean up at process exit
  };
  static Dir dir;
  return dir.path / name;
}

inline std::vector<char> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), {});
}

inline void write_file(const std::string& path,
                       const std::vector<char>& bytes) {
  std::ofstream(path, std::ios::binary)
      .write(bytes.data(), std::streamsize(bytes.size()));
}

// Random non-empty rect inside a width × height level, at most `max_size` on
// each side. Draws w, h, x, y from `rng` in that order.
template <class Rng>
prmd::Rect random_rect(Rng& rng, int width, int height, int max_size) {
  const int w = 1 + int(rng() % unsigned(std::min(width, max_size)));
  const int h = 1 + int(rng() % unsigned(std::min(height, max_size)));
  const int x = int(rng() % unsigned(width - w + 1));
  const int y = int(rng() % unsigned(height - h + 1));
  return prmd::Rect{x, y, w, h};
}

// "{x,y,w,h}" for failure messages.
inline std::string to_string(const prmd::Rect& r) {
  return "{" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
         std::to_string(r.w) + "," + std::to_string(r.h) + "}";
}

// A named .prmd configuration used to parameterise codec tests.
struct Config {
  const char* name;
  prmd::Transform t;
  prmd::Downsampler d;
  prmd::Entropy e;
  bool med = true;    // EncodeOptions::med
  bool ycocg = true;  // EncodeOptions::ycocg
};

inline void PrintTo(const Config& c, std::ostream* os) {
  *os << c.name;
}

inline const std::vector<Config>& prmd_configs() {
  using prmd::Downsampler;
  using prmd::Entropy;
  using prmd::Transform;
  static const std::vector<Config> c = {
      {"haar", Transform::Haar, Downsampler::Box, Entropy::Zstd},
      {"cdf53", Transform::Cdf53, Downsampler::Box, Entropy::Zstd},
      {"laplacian_box", Transform::Laplacian, Downsampler::Box, Entropy::Zstd},
      {"laplacian_gauss", Transform::Laplacian, Downsampler::Gauss,
       Entropy::Zstd},
      {"naive", Transform::Independent, Downsampler::Box, Entropy::Zstd},
      {"haar_range", Transform::Haar, Downsampler::Box, Entropy::Range},
      {"cdf53_raw", Transform::Cdf53, Downsampler::Box, Entropy::Raw},
      // --no-med / --no-ycocg: naive (every level base), a wavelet (base
      // and detail levels) and laplacian (closed-loop color inverse).
      {"naive_no_med", Transform::Independent, Downsampler::Box, Entropy::Zstd,
       false, true},
      {"naive_no_med_no_ycocg", Transform::Independent, Downsampler::Box,
       Entropy::Zstd, false, false},
      {"haar_no_med_no_ycocg_range", Transform::Haar, Downsampler::Box,
       Entropy::Range, false, false},
      {"laplacian_box_no_ycocg", Transform::Laplacian, Downsampler::Box,
       Entropy::Zstd, true, false},
  };
  return c;
}

inline prmd::EncodeOptions options_for(const Config& c,
                                       int levels,
                                       prmd::ColorFormat f,
                                       int tile = 256,
                                       int cblk = 128) {
  prmd::EncodeOptions o;
  o.levels = levels;
  o.color = f;
  o.transform = c.t;
  o.downsampler = c.d;
  o.entropy = c.e;
  o.med = c.med;
  o.ycocg = c.ycocg;
  o.tile = tile;
  o.cblk = cblk;
  return o;
}

}  // namespace testutil
