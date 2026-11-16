// Shared block-coding helpers (YCoCg-R, MED) and the coder factory.

#include "prmd/internal/blockcoder.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace prmd::internal {

// Defined in blockcoder_range.cpp and blockcoder_bytes.cpp.
std::unique_ptr<BlockCoder> make_range_coder();
std::unique_ptr<BlockCoder> make_zstd_coder(int level);
std::unique_ptr<BlockCoder> make_raw_coder();

namespace {

// Level 1: encodes 15-30x faster than 19 at the same size for gray lossless
// files; 19 is ~4% smaller on RGB and 3-12% on lossy (DESIGN.md §7.2).
constexpr int kDefaultZstdLevel = 1;
constexpr int kMaxZstdLevel = 22;

inline size_t sample_index(int x, int y, int w) {
  return size_t(y) * size_t(w) + size_t(x);
}

// MED (LOCO-I) predictor for sample (x, y) from its causal neighbours:
// a = left, b = up, c = up-left. Out-of-block neighbours are not used.
inline int32_t med_pred(const int32_t* px, int w, int x, int y) {
  if (y == 0) {
    return x == 0 ? 0 : px[x - 1];
  }
  const int32_t b = px[sample_index(x, y - 1, w)];
  if (x == 0) {
    return b;
  }
  const int32_t a = px[sample_index(x - 1, y, w)];
  const int32_t c = px[sample_index(x - 1, y - 1, w)];

  // MED is the median of (a, b, a + b - c): c >= max(a, b) gives min(a, b),
  // c <= min(a, b) gives max(a, b), otherwise a + b - c lies between them.
  // Written as min/max so it compiles to conditional selects: the if/else
  // form branches on image data, which the CPU mispredicts on fresh blocks.
  const int32_t mx = std::max(a, b);
  const int32_t mn = std::min(a, b);
  return std::max(mn, std::min(mx, a + b - c));
}

}  // namespace

std::unique_ptr<BlockCoder> make_block_coder(Entropy e, int level) {
  switch (e) {
    case Entropy::Range:
      if (level != 0) {
        throw std::invalid_argument("range coder takes no entropy level");
      }
      return make_range_coder();

    case Entropy::Zstd:
      if (level < 0 || level > kMaxZstdLevel) {
        throw std::invalid_argument("zstd level must be 1..22");
      }
      return make_zstd_coder(level == 0 ? kDefaultZstdLevel : level);

    case Entropy::Raw:
      if (level != 0) {
        throw std::invalid_argument("raw coder takes no entropy level");
      }
      return make_raw_coder();
  }
  throw std::invalid_argument("unknown entropy coder id " +
                              std::to_string(int(e)));
}

void ycocg_r_forward(int32_t* r, int32_t* g, int32_t* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const int32_t co = r[i] - b[i];
    const int32_t t = b[i] + (co >> 1);
    const int32_t cg = g[i] - t;
    const int32_t y = t + (cg >> 1);
    r[i] = y;
    g[i] = co;
    b[i] = cg;
  }
}

void ycocg_r_inverse(int32_t* y, int32_t* co, int32_t* cg, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const int32_t t = y[i] - (cg[i] >> 1);
    const int32_t g = cg[i] + t;
    const int32_t b = t - (co[i] >> 1);
    const int32_t r = b + co[i];
    y[i] = r;
    co[i] = g;
    cg[i] = b;
  }
}

void med_residual(const int32_t* px, int w, int h, int32_t* res) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = sample_index(x, y, w);
      res[i] = px[i] - med_pred(px, w, x, y);
    }
  }
}

void med_reconstruct(const int32_t* res, int w, int h, int32_t* px) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = sample_index(x, y, w);
      px[i] = res[i] + med_pred(px, w, x, y);
    }
  }
}

}  // namespace prmd::internal
