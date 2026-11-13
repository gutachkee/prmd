#pragma once
// Shared helpers for codec tests.

#include <unistd.h>

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "prmd/encoder.h"

namespace prmd {
inline void PrintTo(ColorFormat f, std::ostream* os) { *os << info(f).name; }
}  // namespace prmd

namespace testutil {

inline std::filesystem::path temp_path(const std::string& name) {
  // Per-process directory: ctest runs every test as its own process, in parallel.
  struct Dir {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("prmd_tests_" + std::to_string(::getpid()));
    Dir() { std::filesystem::create_directories(path); }
    ~Dir() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);  // clean up at process exit
    }
  };
  static Dir dir;
  return dir.path / name;
}

// A named .prmd configuration used to parameterise codec tests.
struct Config {
  const char* name;
  prmd::Transform t;
  prmd::Downsampler d;
  prmd::Entropy e;
};

inline void PrintTo(const Config& c, std::ostream* os) { *os << c.name; }

inline const std::vector<Config>& prmd_configs() {
  using prmd::Downsampler;
  using prmd::Entropy;
  using prmd::Transform;
  static const std::vector<Config> c = {
      {"haar", Transform::Haar, Downsampler::Box, Entropy::Zstd},
      {"cdf53", Transform::Cdf53, Downsampler::Box, Entropy::Zstd},
      {"laplacian_box", Transform::Laplacian, Downsampler::Box, Entropy::Zstd},
      {"laplacian_gauss", Transform::Laplacian, Downsampler::Gauss, Entropy::Zstd},
      {"naive", Transform::Independent, Downsampler::Box, Entropy::Zstd},
      {"haar_range", Transform::Haar, Downsampler::Box, Entropy::Range},
      {"cdf53_raw", Transform::Cdf53, Downsampler::Box, Entropy::Raw},
  };
  return c;
}

inline prmd::EncodeOptions options_for(const Config& c, int levels, prmd::ColorFormat f, int tile = 256,
                                       int cblk = 128) {
  prmd::EncodeOptions o;
  o.levels = levels;
  o.color = f;
  o.transform = c.t;
  o.downsampler = c.d;
  o.entropy = c.e;
  o.tile = tile;
  o.cblk = cblk;
  return o;
}

}  // namespace testutil
