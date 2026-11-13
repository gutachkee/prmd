// Shared block-coding helpers (YCoCg-R, MED) and the coder factory.

#include "prmd/internal/blockcoder.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace prmd::internal {

std::unique_ptr<BlockCoder> make_range_coder();
std::unique_ptr<BlockCoder> make_zstd_coder(int level);
std::unique_ptr<BlockCoder> make_raw_coder();

std::unique_ptr<BlockCoder> make_block_coder(Entropy e, int level) {
  switch (e) {
    case Entropy::Range:
      if (level != 0) throw std::invalid_argument("range coder takes no entropy level");
      return make_range_coder();
    case Entropy::Zstd:
      if (level < 0 || level > 22) throw std::invalid_argument("zstd level must be 1..22");
      return make_zstd_coder(level == 0 ? 19 : level);
    case Entropy::Raw:
      if (level != 0) throw std::invalid_argument("raw coder takes no entropy level");
      return make_raw_coder();
  }
  throw std::invalid_argument("unknown entropy coder id " + std::to_string(int(e)));
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

namespace {
inline int32_t med_pred(const int32_t* px, int w, int x, int y) {
  if (y == 0) return x == 0 ? 0 : px[x - 1];
  const int32_t b = px[size_t(y - 1) * size_t(w) + size_t(x)];
  if (x == 0) return b;
  const int32_t a = px[size_t(y) * size_t(w) + size_t(x - 1)];
  const int32_t c = px[size_t(y - 1) * size_t(w) + size_t(x - 1)];
  const int32_t mx = std::max(a, b), mn = std::min(a, b);
  if (c >= mx) return mn;
  if (c <= mn) return mx;
  return a + b - c;
}
}  // namespace

void med_residual(const int32_t* px, int w, int h, int32_t* res) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = size_t(y) * size_t(w) + size_t(x);
      res[i] = px[i] - med_pred(px, w, x, y);
    }
}

void med_reconstruct(const int32_t* res, int w, int h, int32_t* px) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t i = size_t(y) * size_t(w) + size_t(x);
      px[i] = res[i] + med_pred(px, w, x, y);
    }
}

}  // namespace prmd::internal
