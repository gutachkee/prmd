// Level transforms: Haar (S-transform), Cdf53 (reversible 5/3 lifting),
// Laplacian (D + residual).
//
// Integer floor division by 2/4 uses arithmetic right shift (GCC/Clang define
// >> on negative signed values as arithmetic; C++20 mandates it).

#include "prmd/internal/transform.h"

#include <algorithm>
#include <stdexcept>

#include "prmd/internal/parallel.h"

namespace prmd::internal {

namespace {

inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

Rect clip(const Rect& r, Dims d) {
  return intersect(r, Rect{0, 0, d.w, d.h});
}

Dims dims_of(const Region& plane) {
  return Dims{plane.rect.w, plane.rect.h};
}

// A whole plane the caller writes entirely (no zero-fill; see NoInit).
Region whole_plane_uninit(Dims d) {
  return Region(Rect{0, 0, d.w, d.h}, kNoInit);
}

// Sample of a whole plane with edge replication (used for padding odd dims to
// even).
inline int32_t rep(const Region& p, int x, int y) {
  return p.at(clampi(x, 0, p.rect.w - 1), clampi(y, 0, p.rect.h - 1));
}

// Half-resolution rect covering the fine rect r: [x/2, ceil(right/2)) on each
// axis, clipped to the coarse plane.
Rect half_rect(const Rect& r, Dims fine) {
  const int x0 = r.x / 2;
  const int y0 = r.y / 2;
  const int x1 = (r.right() + 1) / 2;
  const int y1 = (r.bottom() + 1) / 2;
  return clip(Rect{x0, y0, x1 - x0, y1 - y0}, half_up(fine));
}

// Inclusive coarse index range [lo, hi] needed to rebuild fine samples [a, b)
// when each fine sample depends on coarse neighbours n-1 .. n+1 (Cdf53 inverse
// and the Laplacian upsampler); m = coarse length.
struct Span {
  int lo = 0;
  int hi = 0;
};

Span coarse_span(int a, int b, int m) {
  return Span{std::max((a >> 1) - 1, 0), std::min(((b - 1) >> 1) + 1, m - 1)};
}

Rect coarse_support(const Rect& r, Dims fine) {
  const Dims c = half_up(fine);
  const Span x = coarse_span(r.x, r.right(), c.w);
  const Span y = coarse_span(r.y, r.bottom(), c.h);
  return Rect{x.lo, y.lo, x.hi - x.lo + 1, y.hi - y.lo + 1};
}

// Common shape of the two wavelet transforms: three half-size detail bands
// (LH, HL, HH).
class WaveletTransform : public LevelTransform {
 public:
  int num_bands() const override { return 3; }
  Band band_kind(int i) const override { return Band(1 + i); }
  Dims band_size(Dims fine) const override { return half_up(fine); }

  Rect band_rect_of_block(const Rect& r, Dims fine) const override {
    return half_rect(r, fine);
  }
};

// ----------------------------------------------------------------------------
// Haar

class HaarTransform final : public WaveletTransform {
 public:
  void forward(const Region& fine,
               Region& coarse,
               std::vector<Region>& bands,
               Pool* pool) const override {
    const Dims c = half_up(dims_of(fine));
    coarse = whole_plane_uninit(c);
    bands.clear();
    for (int b = 0; b < 3; ++b) {
      bands.push_back(whole_plane_uninit(c));
    }

    // Coarse row j depends on fine rows 2j, 2j+1 only. Every sample of
    // coarse and bands is written below.
    parallel_rows(pool, c.h, size_t(8) * size_t(c.w), [&](int j0, int j1) {
      for (int j = j0; j < j1; ++j) {
        for (int i = 0; i < c.w; ++i) {
          const int32_t a = rep(fine, 2 * i, 2 * j);
          const int32_t b = rep(fine, 2 * i + 1, 2 * j);
          const int32_t cc = rep(fine, 2 * i, 2 * j + 1);
          const int32_t d = rep(fine, 2 * i + 1, 2 * j + 1);

          const int32_t l0 = (a + b) >> 1;
          const int32_t h0 = a - b;
          const int32_t l1 = (cc + d) >> 1;
          const int32_t h1 = cc - d;

          coarse.at(i, j) = (l0 + l1) >> 1;
          bands[0].at(i, j) = l0 - l1;         // LH
          bands[1].at(i, j) = (h0 + h1) >> 1;  // HL
          bands[2].at(i, j) = h0 - h1;         // HH
        }
      }
    });
  }

  Rect coarse_needed(const Rect& r, Dims fine) const override {
    return half_rect(r, fine);
  }

  Rect band_needed(const Rect& r, Dims fine) const override {
    return half_rect(r, fine);
  }

  void inverse(const Region& coarse,
               const std::vector<Region>& bands,
               const Rect& r,
               Dims fine,
               Region& out) const override {
    out = Region(r);
    const Rect k = half_rect(r, fine);

    for (int j = k.y; j < k.bottom(); ++j) {
      for (int i = k.x; i < k.right(); ++i) {
        const int32_t LL = coarse.at(i, j);
        const int32_t LH = bands[0].at(i, j);
        const int32_t HL = bands[1].at(i, j);
        const int32_t HH = bands[2].at(i, j);

        const int32_t l0 = LL + ((LH + 1) >> 1);
        const int32_t l1 = l0 - LH;
        const int32_t h0 = HL + ((HH + 1) >> 1);
        const int32_t h1 = h0 - HH;

        const int32_t a = l0 + ((h0 + 1) >> 1);
        const int32_t b = a - h0;
        const int32_t c = l1 + ((h1 + 1) >> 1);
        const int32_t d = c - h1;

        // Write the 2×2 fine pixels of this coefficient that fall inside r.
        const int32_t quad[2][2] = {{a, b}, {c, d}};
        for (int dy = 0; dy < 2; ++dy) {
          const int y = 2 * j + dy;
          if (y < r.y || y >= r.bottom()) {
            continue;
          }
          for (int dx = 0; dx < 2; ++dx) {
            const int x = 2 * i + dx;
            if (x < r.x || x >= r.right()) {
              continue;
            }
            out.at(x, y) = quad[dy][dx];
          }
        }
      }
    }
  }
};

// ----------------------------------------------------------------------------
// Cdf53

// 1D forward 5/3 on an even-length signal (n = 2m), whole-sample symmetric
// extension.
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
// S(k), D(k) are valid for k in [max(k0-1,0), min(k1+1,m-1)], k0 = a/2,
// k1 = (b-1)/2.
template <class SF, class DF, class OF>
void inv53_range(int m, int a, int b, SF S, DF D, OF out) {
  auto even = [&](int k) {
    return S(k) - ((D(k > 0 ? k - 1 : 0) + D(k) + 2) >> 2);
  };

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

class Cdf53Transform final : public WaveletTransform {
 public:
  void forward(const Region& fine,
               Region& coarse,
               std::vector<Region>& bands,
               Pool* pool) const override {
    const Dims c = half_up(dims_of(fine));
    const int pw = 2 * c.w;
    const int ph = 2 * c.h;
    const size_t cw = size_t(c.w);

    // Horizontal pass: rows → L (horizontal low) and H (horizontal high), each
    // c.w wide and ph tall. Rows are independent.
    // Both written entirely by this pass.
    Samples lo_rows = uninitialized_samples(cw * size_t(ph));
    Samples hi_rows = uninitialized_samples(cw * size_t(ph));
    parallel_rows(pool, ph, size_t(2) * size_t(pw), [&](int y0, int y1) {
      std::vector<int32_t> line(static_cast<size_t>(pw));
      std::vector<int32_t> s(cw);
      std::vector<int32_t> d(cw);
      for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < pw; ++x) {
          line[size_t(x)] = rep(fine, x, y);
        }
        fwd53(line.data(), pw, s.data(), d.data());
        std::copy(s.begin(), s.end(), lo_rows.begin() + ptrdiff_t(size_t(y) * cw));
        std::copy(d.begin(), d.end(), hi_rows.begin() + ptrdiff_t(size_t(y) * cw));
      }
    });

    // Vertical pass: fwd53() down every column of `rows` → `lo` and `hi`
    // planes, evaluated row by row over all columns at once (row-major, one
    // sequential stream per input row instead of one cache miss per sample).
    // Same integer formulas as fwd53() with n = ph, m = c.h: first every d
    // (high) row, then every s (low) row, which needs d rows j-1 and j. Rows
    // inside each of the two steps are independent.
    auto vertical = [&](const Samples& rows, Region& lo,
                        Region& hi) {
      auto in = [&](int y) { return rows.data() + size_t(y) * cw; };
      parallel_rows(pool, c.h, size_t(3) * size_t(c.w), [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
          const int32_t* x0 = in(2 * j);
          const int32_t* x1 = in(2 * j + 1);
          const int32_t* x2 = in(2 * j + 2 < ph ? 2 * j + 2 : ph - 2);
          int32_t* hi_row = hi.row(j);
          for (size_t k = 0; k < cw; ++k) {
            hi_row[k] = x1[k] - ((x0[k] + x2[k]) >> 1);
          }
        }
      });
      parallel_rows(pool, c.h, size_t(3) * size_t(c.w), [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j) {
          const int32_t* x0 = in(2 * j);
          const int32_t* dl = hi.row(j > 0 ? j - 1 : 0);
          const int32_t* dj = hi.row(j);
          int32_t* lo_row = lo.row(j);
          for (size_t k = 0; k < cw; ++k) {
            lo_row[k] = x0[k] + ((dl[k] + dj[k] + 2) >> 2);
          }
        }
      });
    };

    // Every sample is written by vertical().
    coarse = whole_plane_uninit(c);
    bands.clear();
    for (int b = 0; b < 3; ++b) {
      bands.push_back(whole_plane_uninit(c));
    }
    vertical(lo_rows, coarse, bands[0]);    // L → LL, LH
    vertical(hi_rows, bands[1], bands[2]);  // H → HL, HH
  }

  Rect coarse_needed(const Rect& r, Dims fine) const override {
    return coarse_support(r, fine);
  }

  Rect band_needed(const Rect& r, Dims fine) const override {
    return coarse_support(r, fine);
  }

  void inverse(const Region& coarse,
               const std::vector<Region>& bands,
               const Rect& r,
               Dims fine,
               Region& out) const override {
    const Dims c = half_up(fine);
    const Rect K = coarse_support(r, fine);

    // Vertical inverse → l (from LL, LH) and h (from HL, HH) over rows
    // r.y .. r.bottom(), columns K. This is inv53_range() down every column,
    // evaluated row by row over all columns at once (row-major) with the same
    // integer formulas: even fine row 2j is S(j) - ((D(j-1) + D(j) + 2) >> 2),
    // odd row 2j+1 is D(j) + ((even(j) + even(j+1)) >> 1), even(j+1) falling
    // back to even(j) on the last coefficient row.
    Region l(Rect{K.x, r.y, K.w, r.h});
    Region h(Rect{K.x, r.y, K.w, r.h});
    const size_t kw = size_t(K.w);
    auto vertical = [&](const Region& S, const Region& D, Region& dst) {
      auto srow = [&](int j) { return S.row(j) + (K.x - S.rect.x); };
      auto drow = [&](int j) { return D.row(j) + (K.x - D.rect.x); };
      for (int y = r.y; y < r.bottom(); ++y) {
        const int j = y >> 1;
        int32_t* o = dst.row(y);
        const int32_t* s0 = srow(j);
        const int32_t* dm = drow(j > 0 ? j - 1 : 0);
        const int32_t* d0 = drow(j);
        if ((y & 1) == 0) {
          for (size_t i = 0; i < kw; ++i) {
            o[i] = s0[i] - ((dm[i] + d0[i] + 2) >> 2);
          }
        } else if (j + 1 < c.h) {
          const int32_t* s1 = srow(j + 1);
          const int32_t* d1 = drow(j + 1);
          for (size_t i = 0; i < kw; ++i) {
            const int32_t e0 = s0[i] - ((dm[i] + d0[i] + 2) >> 2);
            const int32_t e1 = s1[i] - ((d0[i] + d1[i] + 2) >> 2);
            o[i] = d0[i] + ((e0 + e1) >> 1);
          }
        } else {
          for (size_t i = 0; i < kw; ++i) {
            const int32_t e0 = s0[i] - ((dm[i] + d0[i] + 2) >> 2);
            o[i] = d0[i] + ((e0 + e0) >> 1);
          }
        }
      }
    };
    vertical(coarse, bands[0], l);
    vertical(bands[1], bands[2], h);

    // Horizontal inverse per row.
    out = Region(r);
    for (int y = r.y; y < r.bottom(); ++y) {
      inv53_range(
          c.w, r.x, r.right(), [&](int k) { return l.at(k, y); },
          [&](int k) { return h.at(k, y); },
          [&](int x, int32_t v) { out.at(x, y) = v; });
    }
  }
};

// ----------------------------------------------------------------------------
// Laplacian

// 1D 2× upsampler, phase-matched to the downsampler. c(n) valid for n in
// [0, cn).
template <class CF>
inline int32_t up1(Downsampler phase, int i, int cn, CF c) {
  const int n = i >> 1;
  const bool even = (i & 1) == 0;

  if (phase == Downsampler::Gauss) {
    if (even) {
      return c(n);
    }
    return (c(n) + c(std::min(n + 1, cn - 1)) + 1) >> 1;
  }

  // Box: coarse samples sit between fine pixel pairs.
  if (even) {
    return (3 * c(n) + c(std::max(n - 1, 0)) + 2) >> 2;
  }
  return (3 * c(n) + c(std::min(n + 1, cn - 1)) + 2) >> 2;
}

// U(coarse) over fine rect r. `coarse` must cover coarse_support(r, fine).
// Both passes are row-independent, so `pool` splits them by rows.
void upsample(Downsampler down,
              const Region& coarse,
              const Rect& r,
              Dims fine,
              Region& out,
              Pool* pool) {
  const Dims c = half_up(fine);
  const Rect K = coarse_support(r, fine);

  // Horizontal pass over the needed coarse rows. Both passes write every
  // sample of tmp and out.
  Region tmp(Rect{r.x, K.y, r.w, K.h}, kNoInit);
  parallel_rows(pool, K.h, size_t(2) * size_t(r.w), [&](int k0, int k1) {
    for (int j = K.y + k0; j < K.y + k1; ++j) {
      for (int x = r.x; x < r.right(); ++x) {
        tmp.at(x, j) =
            up1(down, x, c.w, [&](int n) { return coarse.at(n, j); });
      }
    }
  });

  // Vertical pass.
  out = Region(r, kNoInit);
  parallel_rows(pool, r.h, size_t(2) * size_t(r.w), [&](int y0, int y1) {
    for (int y = r.y + y0; y < r.y + y1; ++y) {
      for (int x = r.x; x < r.right(); ++x) {
        out.at(x, y) = up1(down, y, c.h, [&](int n) { return tmp.at(x, n); });
      }
    }
  });
}

class LaplacianTransform final : public LevelTransform {
 public:
  explicit LaplacianTransform(Downsampler d) : down_(d) {}

  int num_bands() const override { return 1; }
  Band band_kind(int) const override { return Band::Residual; }
  Dims band_size(Dims fine) const override { return fine; }

  Rect band_rect_of_block(const Rect& r, Dims fine) const override {
    return clip(r, fine);
  }

  void forward(const Region& fine,
               Region& coarse,
               std::vector<Region>& bands,
               Pool* pool) const override {
    coarse = downsample(fine, down_, pool);

    Region up;
    upsample(down_, coarse, fine.rect, dims_of(fine), up, pool);

    bands.clear();
    bands.push_back(Region(fine.rect, kNoInit));  // written entirely below
    parallel_samples(pool, fine.v.size(), [&](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        bands[0].v[i] = fine.v[i] - up.v[i];
      }
    });
  }

  Rect coarse_needed(const Rect& r, Dims fine) const override {
    return coarse_support(r, fine);
  }

  Rect band_needed(const Rect& r, Dims fine) const override {
    return clip(r, fine);
  }

  void inverse(const Region& coarse,
               const std::vector<Region>& bands,
               const Rect& r,
               Dims fine,
               Region& out) const override {
    upsample(down_, coarse, r, fine, out, nullptr);
    for (int y = r.y; y < r.bottom(); ++y) {
      for (int x = r.x; x < r.right(); ++x) {
        out.at(x, y) += bands[0].at(x, y);
      }
    }
  }

 private:
  Downsampler down_;
};

inline int reflect101(int i, int n) {
  if (n == 1) {
    return 0;
  }
  while (i < 0 || i >= n) {
    if (i < 0) {
      i = -i;
    }
    if (i >= n) {
      i = 2 * n - 2 - i;
    }
  }
  return i;
}

}  // namespace

void copy_overlap(const Region& src, Region& dst) {
  const Rect o = intersect(src.rect, dst.rect);
  for (int y = o.y; y < o.bottom(); ++y) {
    const int32_t* row = src.row(y);
    std::copy(row + (o.x - src.rect.x), row + (o.right() - src.rect.x),
              dst.row(y) + (o.x - dst.rect.x));
  }
}

Region downsample_box(const Region& fine, Pool* pool) {
  const Dims c = half_up(dims_of(fine));
  Region out = whole_plane_uninit(c);  // every sample written below

  parallel_rows(pool, c.h, size_t(5) * size_t(c.w), [&](int j0, int j1) {
    for (int j = j0; j < j1; ++j) {
      for (int i = 0; i < c.w; ++i) {
        const int32_t sum =
            rep(fine, 2 * i, 2 * j) + rep(fine, 2 * i + 1, 2 * j) +
            rep(fine, 2 * i, 2 * j + 1) + rep(fine, 2 * i + 1, 2 * j + 1);
        out.at(i, j) = (sum + 2) >> 2;
      }
    }
  });
  return out;
}

Region downsample_gauss(const Region& fine, Pool* pool) {
  static constexpr int kTap[5] = {1, 4, 6, 4, 1};
  const int fw = fine.rect.w;
  const int fh = fine.rect.h;
  const Dims c = half_up(Dims{fw, fh});

  // Horizontal pass: c.w columns, every fine row.
  Region tmp(Rect{0, 0, c.w, fh}, kNoInit);  // every sample written
  parallel_rows(pool, fh, size_t(6) * size_t(c.w), [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      for (int i = 0; i < c.w; ++i) {
        int32_t s = 0;
        for (int t = -2; t <= 2; ++t) {
          s += kTap[t + 2] * fine.at(reflect101(2 * i + t, fw), y);
        }
        tmp.at(i, y) = s;
      }
    }
  });

  // Vertical pass and normalization by 16 × 16.
  Region out = whole_plane_uninit(c);
  parallel_rows(pool, c.h, size_t(6) * size_t(c.w), [&](int j0, int j1) {
    for (int j = j0; j < j1; ++j) {
      for (int i = 0; i < c.w; ++i) {
        int32_t s = 0;
        for (int t = -2; t <= 2; ++t) {
          s += kTap[t + 2] * tmp.at(i, reflect101(2 * j + t, fh));
        }
        out.at(i, j) = (s + 128) >> 8;
      }
    }
  });
  return out;
}

Region downsample(const Region& fine, Downsampler d, Pool* pool) {
  return d == Downsampler::Gauss ? downsample_gauss(fine, pool)
                                 : downsample_box(fine, pool);
}

void laplacian_upsample(Downsampler d,
                        const Region& coarse,
                        Dims fine,
                        Region& out,
                        Pool* pool) {
  upsample(d, coarse, Rect{0, 0, fine.w, fine.h}, fine, out, pool);
}

std::unique_ptr<LevelTransform> make_transform(Transform t, Downsampler d) {
  switch (t) {
    case Transform::Haar:
      return std::make_unique<HaarTransform>();
    case Transform::Cdf53:
      return std::make_unique<Cdf53Transform>();
    case Transform::Laplacian:
      return std::make_unique<LaplacianTransform>(d);
    case Transform::Independent:
      break;
  }
  throw std::invalid_argument(
      "make_transform: no level transform for this mode");
}

}  // namespace prmd::internal
