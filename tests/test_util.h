#pragma once
// Shared helpers for codec tests.

#include <unistd.h>

#include <algorithm>
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

inline std::filesystem::path temp_path(const std::string& name) {
  // Per-process directory: ctest runs every test as its own process, in
  // parallel.
  struct Dir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("prmd_tests_" + std::to_string(::getpid()));

    Dir() { std::filesystem::create_directories(path); }

    ~Dir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);  // clean up at process exit
    }
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
