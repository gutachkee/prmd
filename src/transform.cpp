// Level transforms: Haar (S-transform), Cdf53 (reversible 5/3 lifting), Laplacian (D + residual).
//
// Integer floor division by 2/4 uses arithmetic right shift (GCC/Clang define >> on negative
// signed values as arithmetic; C++20 mandates it).

#include "prmd/internal/transform.h"

#include <algorithm>
#include <stdexcept>

namespace prmd::internal {

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

Rect clip(const Rect& r, Dims d) { return intersect(r, Rect{0, 0, d.w, d.h}); }

// Sample of a whole plane with edge replication (used for padding odd dims to even).
inline int32_t rep(const Region& p, int x, int y) {
  return p.at(clampi(x, 0, p.rect.w - 1), clampi(y, 0, p.rect.h - 1));
}

// ------------------------------------------------------------------------------ Haar

class HaarTransform final : public LevelTransform {
 public:
  int num_bands() const override { return 3; }
  Band band_kind(int i) const override { return Band(1 + i); }
  Dims band_size(Dims fine) const override { return half_up(fine); }

  Rect band_rect_of_block(const Rect& r, Dims fine) const override {
    return clip(Rect{r.x / 2, r.y / 2, (r.right() + 1) / 2 - r.x / 2, (r.bottom() + 1) / 2 - r.y / 2},
                half_up(fine));
  }

  void forward(const Region& fine, Region& coarse, std::vector<Region>& bands) const override {
    const Dims c = half_up(Dims{fine.rect.w, fine.rect.h});
    coarse = Region(Rect{0, 0, c.w, c.h});
    bands.assign(3, Region(Rect{0, 0, c.w, c.h}));
    for (int j = 0; j < c.h; ++j) {
      for (int i = 0; i < c.w; ++i) {
        const int32_t a = rep(fine, 2 * i, 2 * j), b = rep(fine, 2 * i + 1, 2 * j);
        const int32_t cc = rep(fine, 2 * i, 2 * j + 1), d = rep(fine, 2 * i + 1, 2 * j + 1);
        const int32_t l0 = (a + b) >> 1, h0 = a - b;
        const int32_t l1 = (cc + d) >> 1, h1 = cc - d;
        coarse.at(i, j) = (l0 + l1) >> 1;
        bands[0].at(i, j) = l0 - l1;  // LH
        bands[1].at(i, j) = (h0 + h1) >> 1;  // HL
        bands[2].at(i, j) = h0 - h1;  // HH
      }
    }
  }

  Rect coarse_needed(const Rect& r, Dims fine) const override { return band_rect_of_block(r, fine); }
  Rect band_needed(const Rect& r, Dims fine) const override { return band_rect_of_block(r, fine); }

  void inverse(const Region& coarse, const std::vector<Region>& bands, const Rect& r, Dims fine,
               Region& out) const override {
    out = Region(r);
    const Rect k = band_rect_of_block(r, fine);
    for (int j = k.y; j < k.bottom(); ++j) {
      for (int i = k.x; i < k.right(); ++i) {
        const int32_t LL = coarse.at(i, j), LH = bands[0].at(i, j);
        const int32_t HL = bands[1].at(i, j), HH = bands[2].at(i, j);
        const int32_t l0 = LL + ((LH + 1) >> 1), l1 = l0 - LH;
        const int32_t h0 = HL + ((HH + 1) >> 1), h1 = h0 - HH;
        const int32_t a = l0 + ((h0 + 1) >> 1), b = a - h0;
        const int32_t c = l1 + ((h1 + 1) >> 1), d = c - h1;
        const int32_t v[2][2] = {{a, b}, {c, d}};
        for (int dy = 0; dy < 2; ++dy) {
          const int y = 2 * j + dy;
          if (y < r.y || y >= r.bottom()) continue;
          for (int dx = 0; dx < 2; ++dx) {
            const int x = 2 * i + dx;
            if (x < r.x || x >= r.right()) continue;
            out.at(x, y) = v[dy][dx];
          }
        }
      }
    }
  }
};

// ------------------------------------------------------------------------------ Cdf53

// 1D forward 5/3 on an even-length signal (n = 2m), whole-sample symmetric extension.
void fwd53(const int32_t* x, int n, int32_t* s, int32_t* d) {
  const int m = n / 2;
  for (int k = 0; k < m; ++k) {
    const int32_t right = x[2 * k + 2 < n ? 2 * k + 2 : n - 2];
    d[k] = x[2 * k + 1] - ((x[2 * k] + right) >> 1);
  }
  for (int k = 0; k < m; ++k) {
    const int32_t dl = d[k > 0 ? k - 1 : 0];
    s[k] = x[2 * k] + ((dl + d[k] + 2) >> 2);
  }
}

// 1D inverse 5/3 over output range [a, b) of a length-2m signal.
// S(k), D(k) are valid for k in [max(k0-1,0), min(k1+1,m-1)], k0 = a/2, k1 = (b-1)/2.
template <class SF, class DF, class OF>
void inv53_range(int m, int a, int b, SF S, DF D, OF out) {
  auto even = [&](int k) { return S(k) - ((D(k > 0 ? k - 1 : 0) + D(k) + 2) >> 2); };
  for (int i = a; i < b; ++i) {
    const int k = i >> 1;
    if ((i & 1) == 0) {
      out(i, even(k));
    } else {
      const int32_t e0 = even(k);
      const int32_t e1 = k + 1 < m ? even(k + 1) : e0;
      out(i, D(k) + ((e0 + e1) >> 1));
    }
  }
}

// Coefficient index range (inclusive) needed to rebuild fine samples [a, b) of length 2m.
inline void need53(int a, int b, int m, int& lo, int& hi) {
  lo = std::max((a >> 1) - 1, 0);
  hi = std::min(((b - 1) >> 1) + 1, m - 1);
}

class Cdf53Transform final : public LevelTransform {
 public:
  int num_bands() const override { return 3; }
  Band band_kind(int i) const override { return Band(1 + i); }
  Dims band_size(Dims fine) const override { return half_up(fine); }

  Rect band_rect_of_block(const Rect& r, Dims fine) const override {
    return clip(Rect{r.x / 2, r.y / 2, (r.right() + 1) / 2 - r.x / 2, (r.bottom() + 1) / 2 - r.y / 2},
                half_up(fine));
  }

  void forward(const Region& fine, Region& coarse, std::vector<Region>& bands) const override {
    const Dims c = half_up(Dims{fine.rect.w, fine.rect.h});
    const int pw = 2 * c.w, ph = 2 * c.h;
    // Rows: L (horizontal low) and H (horizontal high), each c.w wide, ph tall.
    std::vector<int32_t> L(size_t(c.w) * size_t(ph)), H(size_t(c.w) * size_t(ph));
    std::vector<int32_t> line(size_t(std::max(pw, ph))), s(size_t(std::max(c.w, c.h))),
        d(size_t(std::max(c.w, c.h)));
    for (int y = 0; y < ph; ++y) {
      for (int x = 0; x < pw; ++x) line[size_t(x)] = rep(fine, x, y);
      fwd53(line.data(), pw, s.data(), d.data());
      for (int k = 0; k < c.w; ++k) {
        L[size_t(y) * size_t(c.w) + size_t(k)] = s[size_t(k)];
        H[size_t(y) * size_t(c.w) + size_t(k)] = d[size_t(k)];
      }
    }
    coarse = Region(Rect{0, 0, c.w, c.h});
    bands.assign(3, Region(Rect{0, 0, c.w, c.h}));
    for (int k = 0; k < c.w; ++k) {
      for (int pass = 0; pass < 2; ++pass) {
        const std::vector<int32_t>& src = pass == 0 ? L : H;
        for (int y = 0; y < ph; ++y) line[size_t(y)] = src[size_t(y) * size_t(c.w) + size_t(k)];
        fwd53(line.data(), ph, s.data(), d.data());
        Region& lo = pass == 0 ? coarse : bands[1];  // L→LL, H→HL
        Region& hi = pass == 0 ? bands[0] : bands[2];  // L→LH, H→HH
        for (int j = 0; j < c.h; ++j) {
          lo.at(k, j) = s[size_t(j)];
          hi.at(k, j) = d[size_t(j)];
        }
      }
    }
  }

  Rect coarse_needed(const Rect& r, Dims fine) const override { return needed(r, fine); }
  Rect band_needed(const Rect& r, Dims fine) const override { return needed(r, fine); }

  void inverse(const Region& coarse, const std::vector<Region>& bands, const Rect& r, Dims fine,
               Region& out) const override {
    const Dims c = half_up(fine);
    const Rect K = needed(r, fine);
    // Vertical inverse per coefficient column → l (from LL, LH) and h (from HL, HH) over rows r.y..
    Region l(Rect{K.x, r.y, K.w, r.h}), h(Rect{K.x, r.y, K.w, r.h});
    for (int k = K.x; k < K.right(); ++k) {
      inv53_range(
          c.h, r.y, r.bottom(), [&](int j) { return coarse.at(k, j); },
          [&](int j) { return bands[0].at(k, j); }, [&](int y, int32_t v) { l.at(k, y) = v; });
      inv53_range(
          c.h, r.y, r.bottom(), [&](int j) { return bands[1].at(k, j); },
          [&](int j) { return bands[2].at(k, j); }, [&](int y, int32_t v) { h.at(k, y) = v; });
    }
    out = Region(r);
    for (int y = r.y; y < r.bottom(); ++y) {
      inv53_range(
          c.w, r.x, r.right(), [&](int k) { return l.at(k, y); }, [&](int k) { return h.at(k, y); },
          [&](int x, int32_t v) { out.at(x, y) = v; });
    }
  }

 private:
  static Rect needed(const Rect& r, Dims fine) {
    const Dims c = half_up(fine);
    int x0, x1, y0, y1;
    need53(r.x, r.right(), c.w, x0, x1);
    need53(r.y, r.bottom(), c.h, y0, y1);
    return Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
  }
};

// ------------------------------------------------------------------------------ Laplacian

// 1D 2× upsampler, phase-matched to the downsampler. c(n) valid for n in [0, cn).
template <class CF>
inline int32_t up1(Downsampler phase, int i, int cn, CF c) {
  const int n = i >> 1;
  if (phase == Downsampler::Gauss) {
    if ((i & 1) == 0) return c(n);
    return (c(n) + c(std::min(n + 1, cn - 1)) + 1) >> 1;
  }
  // Box: coarse samples sit between fine pixel pairs.
  if ((i & 1) == 0) return (3 * c(n) + c(std::max(n - 1, 0)) + 2) >> 2;
  return (3 * c(n) + c(std::min(n + 1, cn - 1)) + 2) >> 2;
}

class LaplacianTransform final : public LevelTransform {
 public:
  explicit LaplacianTransform(Downsampler d) : down_(d) {}

  int num_bands() const override { return 1; }
  Band band_kind(int) const override { return Band::Residual; }
  Dims band_size(Dims fine) const override { return fine; }
  Rect band_rect_of_block(const Rect& r, Dims fine) const override { return clip(r, fine); }

  void forward(const Region& fine, Region& coarse, std::vector<Region>& bands) const override {
    coarse = down_ == Downsampler::Gauss ? downsample_gauss(fine) : downsample_box(fine);
    const Dims fd{fine.rect.w, fine.rect.h};
    Region up;
    upsample(coarse, fine.rect, fd, up);
    bands.assign(1, Region(fine.rect));
    for (size_t i = 0; i < fine.v.size(); ++i) bands[0].v[i] = fine.v[i] - up.v[i];
  }

  Rect coarse_needed(const Rect& r, Dims fine) const override {
    const Dims c = half_up(fine);
    const int x0 = std::max((r.x >> 1) - 1, 0), x1 = std::min(((r.right() - 1) >> 1) + 1, c.w - 1);
    const int y0 = std::max((r.y >> 1) - 1, 0), y1 = std::min(((r.bottom() - 1) >> 1) + 1, c.h - 1);
    return Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
  }
  Rect band_needed(const Rect& r, Dims fine) const override { return clip(r, fine); }

  void inverse(const Region& coarse, const std::vector<Region>& bands, const Rect& r, Dims fine,
               Region& out) const override {
    upsample(coarse, r, fine, out);
    for (int y = r.y; y < r.bottom(); ++y)
      for (int x = r.x; x < r.right(); ++x) out.at(x, y) += bands[0].at(x, y);
  }

 private:
  // U(coarse) over fine rect r. `coarse` must cover coarse_needed(r).
  void upsample(const Region& coarse, const Rect& r, Dims fine, Region& out) const {
    const Dims c = half_up(fine);
    const Rect K = coarse_needed(r, fine);
    Region tmp(Rect{r.x, K.y, r.w, K.h});  // horizontal pass over the needed coarse rows
    for (int j = K.y; j < K.bottom(); ++j)
      for (int x = r.x; x < r.right(); ++x)
        tmp.at(x, j) = up1(down_, x, c.w, [&](int n) { return coarse.at(n, j); });
    out = Region(r);
    for (int y = r.y; y < r.bottom(); ++y)
      for (int x = r.x; x < r.right(); ++x)
        out.at(x, y) = up1(down_, y, c.h, [&](int n) { return tmp.at(x, n); });
  }

  Downsampler down_;
};

inline int reflect101(int i, int n) {
  if (n == 1) return 0;
  while (i < 0 || i >= n) {
    if (i < 0) i = -i;
    if (i >= n) i = 2 * n - 2 - i;
  }
  return i;
}

}  // namespace

void copy_overlap(const Region& src, Region& dst) {
  const Rect o = intersect(src.rect, dst.rect);
  for (int y = o.y; y < o.bottom(); ++y)
    std::copy(src.row(y) + (o.x - src.rect.x), src.row(y) + (o.right() - src.rect.x),
              dst.row(y) + (o.x - dst.rect.x));
}

Region downsample_box(const Region& fine) {
  const Dims c = half_up(Dims{fine.rect.w, fine.rect.h});
  Region out(Rect{0, 0, c.w, c.h});
  for (int j = 0; j < c.h; ++j)
    for (int i = 0; i < c.w; ++i)
      out.at(i, j) = (rep(fine, 2 * i, 2 * j) + rep(fine, 2 * i + 1, 2 * j) + rep(fine, 2 * i, 2 * j + 1) +
                      rep(fine, 2 * i + 1, 2 * j + 1) + 2) >>
                     2;
  return out;
}

Region downsample_gauss(const Region& fine) {
  const int fw = fine.rect.w, fh = fine.rect.h;
  const Dims c = half_up(Dims{fw, fh});
  static constexpr int kTap[5] = {1, 4, 6, 4, 1};
  Region tmp(Rect{0, 0, c.w, fh});
  for (int y = 0; y < fh; ++y)
    for (int i = 0; i < c.w; ++i) {
      int32_t s = 0;
      for (int t = -2; t <= 2; ++t) s += kTap[t + 2] * fine.at(reflect101(2 * i + t, fw), y);
      tmp.at(i, y) = s;
    }
  Region out(Rect{0, 0, c.w, c.h});
  for (int j = 0; j < c.h; ++j)
    for (int i = 0; i < c.w; ++i) {
      int32_t s = 0;
      for (int t = -2; t <= 2; ++t) s += kTap[t + 2] * tmp.at(i, reflect101(2 * j + t, fh));
      out.at(i, j) = (s + 128) >> 8;
    }
  return out;
}

std::unique_ptr<LevelTransform> make_transform(Transform t, Downsampler d) {
  switch (t) {
    case Transform::Haar: return std::make_unique<HaarTransform>();
    case Transform::Cdf53: return std::make_unique<Cdf53Transform>();
    case Transform::Laplacian: return std::make_unique<LaplacianTransform>(d);
    case Transform::Independent: break;
  }
  throw std::invalid_argument("make_transform: no level transform for this mode");
}

}  // namespace prmd::internal
