#pragma once
// Encoder configurations for the thread-count tests (test_threads.cpp): every
// mode, gray and RGB, zstd levels, range and raw, lossy steps, --no-med /
// --no-ycocg, tiny and odd sizes, single-block images and many-block images.
//
// The golden hashes in test_threads.cpp were produced by encoding exactly
// these cases with the serial encoder of commit fc0a629 (before threading),
// so the matrix and the image generator must not change without regenerating
// them. Everything here is integer-only (no libm), so the images, and hence
// the hashes, are the same on every platform.

#include <cstdint>
#include <string>
#include <vector>

#include "prmd/encoder.h"
#include "prmd/image.h"

namespace threadmatrix {

// Smooth ramps, a disc with a sharp edge, thin lines and ±4 noise, from a
// xorshift32 stream. `noise` = 1 replaces all of it with uniform 0..255 noise
// (worst case for the coders: large detail values everywhere).
inline prmd::Image pattern_image(int w,
                                 int h,
                                 prmd::ColorFormat f,
                                 uint32_t seed,
                                 bool noise = false) {
  prmd::Image img(w, h, f);
  const int C = img.channels();
  uint32_t s = seed * 2654435761u + 0x9E3779B9u;
  auto next = [&s] {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  };
  const int cx = int(next() % uint32_t(w));
  const int cy = int(next() % uint32_t(h));
  const int rad = 1 + int(next() % uint32_t(1 + (w < h ? w : h) / 3));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < C; ++c) {
        int v;
        if (noise) {
          v = int(next() & 255u);
        } else {
          v = (x * (3 + c) + y * (5 - c)) / 4 % 200 + 20;
          if ((x / 23 + y / 17) % 2) {
            v = 255 - v;
          }
          if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < rad * rad) {
            v = 230 - 40 * c;
          }
          if (x % 37 < 2) {
            v = 20;
          }
          v += int(next() % 9) - 4;
        }
        img.row(y)[size_t(x) * size_t(C) + size_t(c)] =
            uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
      }
    }
  }
  return img;
}

struct Case {
  std::string name;  // unique, stable: the key of the golden table
  int w = 0;
  int h = 0;
  prmd::ColorFormat src_format = prmd::ColorFormat::Gray8;
  bool noise = false;
  prmd::EncodeOptions opts;  // threads left at its default
};

// FNV-1a 64 of a byte string.
inline uint64_t fnv1a64(const char* data, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= uint8_t(data[i]);
    h *= 0x100000001b3ull;
  }
  return h;
}

inline std::vector<Case> cases() {
  using prmd::ColorFormat;
  using prmd::Downsampler;
  using prmd::Entropy;
  using prmd::Transform;

  struct Mode {
    const char* name;
    Transform t;
    Downsampler d;
    Entropy e;
    bool med;
    bool ycocg;
  };
  const Mode modes[] = {
      {"haar", Transform::Haar, Downsampler::Box, Entropy::Zstd, true, true},
      {"cdf53", Transform::Cdf53, Downsampler::Box, Entropy::Zstd, true, true},
      {"lapbox", Transform::Laplacian, Downsampler::Box, Entropy::Zstd, true,
       true},
      {"lapgauss", Transform::Laplacian, Downsampler::Gauss, Entropy::Zstd,
       true, true},
      {"naive", Transform::Independent, Downsampler::Box, Entropy::Zstd, true,
       true},
      {"naivegauss", Transform::Independent, Downsampler::Gauss, Entropy::Zstd,
       true, true},
      {"haar_range", Transform::Haar, Downsampler::Box, Entropy::Range, true,
       true},
      {"cdf53_raw", Transform::Cdf53, Downsampler::Box, Entropy::Raw, true,
       true},
      {"naive_nomed", Transform::Independent, Downsampler::Box, Entropy::Zstd,
       false, true},
      {"naive_nomed_noycocg", Transform::Independent, Downsampler::Box,
       Entropy::Zstd, false, false},
      {"haar_nomed_noycocg_range", Transform::Haar, Downsampler::Box,
       Entropy::Range, false, false},
      {"lapbox_noycocg", Transform::Laplacian, Downsampler::Box, Entropy::Zstd,
       true, false},
  };
  struct Size {
    int w, h, levels, tile, cblk;
  };
  // 1×1 … 17×1: tiny, below one block. 100×100: one block per level.
  // 300×200 at cblk 32: many blocks and partial edge tiles. 257×129: odd
  // dims at every level. 1000×777: several row stripes per level.
  const Size sizes[] = {
      {1, 1, 6, 256, 128},    {3, 2, 6, 256, 128},   {17, 1, 6, 256, 128},
      {1, 23, 4, 256, 128},   {100, 100, 5, 256, 128}, {300, 200, 5, 64, 32},
      {257, 129, 5, 256, 128}, {1000, 777, 5, 256, 128},
  };

  std::vector<Case> out;
  auto add = [&](std::string name, int w, int h, ColorFormat src_format,
                 ColorFormat stored, const prmd::EncodeOptions& base,
                 bool noise = false) {
    Case c;
    c.name = std::move(name);
    c.w = w;
    c.h = h;
    c.src_format = src_format;
    c.noise = noise;
    c.opts = base;
    c.opts.color = stored;
    out.push_back(std::move(c));
  };

  for (const Mode& m : modes) {
    for (const Size& s : sizes) {
      for (const ColorFormat f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
        prmd::EncodeOptions o;
        o.levels = s.levels;
        o.tile = s.tile;
        o.cblk = s.cblk;
        o.transform = m.t;
        o.downsampler = m.d;
        o.entropy = m.e;
        o.med = m.med;
        o.ycocg = m.ycocg;
        add(std::string(m.name) + "_" + std::to_string(s.w) + "x" +
                std::to_string(s.h) + "_" + prmd::info(f).name,
            s.w, s.h, f, f, o);
      }
    }
  }

  // Lossy: every lossy mode, steps 1 (identity), 3 and 8, gray and RGB.
  for (const Mode& m : modes) {
    if (m.t == Transform::Independent || m.e == Entropy::Raw) {
      continue;
    }
    for (const int q : {1, 3, 8}) {
      for (const ColorFormat f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
        prmd::EncodeOptions o;
        o.transform = m.t;
        o.downsampler = m.d;
        o.entropy = m.e;
        o.med = m.med;
        o.ycocg = m.ycocg;
        o.quant = q;
        o.tile = 64;
        o.cblk = 32;
        add(std::string(m.name) + "_q" + std::to_string(q) + "_" +
                prmd::info(f).name,
            301, 203, f, f, o);
      }
    }
  }

  // zstd levels, including 19, on a small image.
  for (const int zl : {3, 19, 22}) {
    for (const ColorFormat f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
      prmd::EncodeOptions o;
      o.entropy_level = zl;
      o.tile = 64;
      o.cblk = 32;
      add("haar_zstd" + std::to_string(zl) + "_" + prmd::info(f).name, 240,
          170, f, f, o);
      o.transform = Transform::Cdf53;
      o.quant = 4;
      add("cdf53_q4_zstd" + std::to_string(zl) + "_" + prmd::info(f).name,
          240, 170, f, f, o);
    }
  }

  // Geometry: 1 level (base only), many levels, smallest blocks, one huge
  // block, tile = cblk.
  {
    prmd::EncodeOptions o;
    o.levels = 1;
    add("haar_levels1", 300, 200, ColorFormat::Gray8, ColorFormat::Gray8, o);
    o.levels = 8;
    o.tile = 16;
    o.cblk = 16;
    add("haar_levels8_cblk16", 300, 200, ColorFormat::RGB8, ColorFormat::RGB8,
        o);
    o.transform = Transform::Cdf53;
    add("cdf53_levels8_cblk16", 300, 200, ColorFormat::RGB8, ColorFormat::RGB8,
        o);
    o.transform = Transform::Laplacian;
    o.quant = 2;
    add("lapbox_q2_levels8_cblk16", 300, 200, ColorFormat::RGB8,
        ColorFormat::RGB8, o);
    prmd::EncodeOptions big;
    big.levels = 3;
    big.tile = 4096;
    big.cblk = 4096;
    add("haar_one_block_per_level", 517, 301, ColorFormat::Gray8,
        ColorFormat::Gray8, big);
  }

  // Colour conversion inside the encoder (RGB source stored gray, gray
  // source stored RGB).
  {
    prmd::EncodeOptions o;
    add("convert_rgb_to_gray", 333, 222, ColorFormat::RGB8, ColorFormat::Gray8,
        o);
    o.transform = Transform::Laplacian;
    add("convert_gray_to_rgb_lap", 333, 222, ColorFormat::Gray8,
        ColorFormat::RGB8, o);
  }

  // Full-range noise: the largest detail values.
  for (const Mode& m : modes) {
    prmd::EncodeOptions o;
    o.transform = m.t;
    o.downsampler = m.d;
    o.entropy = m.e;
    o.med = m.med;
    o.ycocg = m.ycocg;
    o.tile = 64;
    o.cblk = 32;
    add(std::string(m.name) + "_noise_rgb", 150, 97, ColorFormat::RGB8,
        ColorFormat::RGB8, o, true);
  }
  return out;
}

// The source image of a case.
inline prmd::Image source(const Case& c) {
  return pattern_image(c.w, c.h, c.src_format,
                       uint32_t(c.w) * 31u + uint32_t(c.h), c.noise);
}

}  // namespace threadmatrix
