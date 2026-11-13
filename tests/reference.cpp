#include "reference.h"

#include <cmath>
#include <random>

using prmd::ColorFormat;
using prmd::Downsampler;
using prmd::Image;
using prmd::Transform;

namespace ref {
namespace {

using Grid = std::vector<std::vector<long>>;  // [y][x]

long floor_div(long a, long b) {
  long q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) {
    --q;
  }
  return q;
}

long get_rep(const Grid& g, long x, long y) {
  const long h = long(g.size()), w = long(g[0].size());
  if (x >= w) {
    x = w - 1;
  }
  if (y >= h) {
    y = h - 1;
  }
  return g[size_t(y)][size_t(x)];
}

Grid haar_down(const Grid& g) {
  const size_t h = (g.size() + 1) / 2, w = (g[0].size() + 1) / 2;
  Grid out(h, std::vector<long>(w));
  for (size_t j = 0; j < h; ++j) {
    for (size_t i = 0; i < w; ++i) {
      const long x0 = long(2 * i);
      const long y0 = long(2 * j);
      const long a = get_rep(g, x0, y0);
      const long b = get_rep(g, x0 + 1, y0);
      const long c = get_rep(g, x0, y0 + 1);
      const long d = get_rep(g, x0 + 1, y0 + 1);
      out[j][i] = floor_div(floor_div(a + b, 2) + floor_div(c + d, 2), 2);
    }
  }
  return out;
}

Grid box_down(const Grid& g) {
  const size_t h = (g.size() + 1) / 2, w = (g[0].size() + 1) / 2;
  Grid out(h, std::vector<long>(w));
  for (size_t j = 0; j < h; ++j) {
    for (size_t i = 0; i < w; ++i) {
      long s = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          s += get_rep(g, long(2 * i) + dx, long(2 * j) + dy);
        }
      }
      out[j][i] = floor_div(s + 2, 4);
    }
  }
  return out;
}

long mirror101(long i, long n) {
  if (n == 1) {
    return 0;
  }
  const long period = 2 * (n - 1);
  i = ((i % period) + period) % period;
  return i < n ? i : period - i;
}

Grid gauss_down(const Grid& g) {
  const long H = long(g.size()), W = long(g[0].size());
  const long h = (H + 1) / 2, w = (W + 1) / 2;
  const long k[5] = {1, 4, 6, 4, 1};
  Grid out(static_cast<size_t>(h), std::vector<long>(static_cast<size_t>(w)));
  for (long j = 0; j < h; ++j) {
    for (long i = 0; i < w; ++i) {
      long s = 0;
      for (long dy = -2; dy <= 2; ++dy) {
        for (long dx = -2; dx <= 2; ++dx) {
          s += k[dy + 2] * k[dx + 2] *
               g[size_t(mirror101(2 * j + dy, H))]
                [size_t(mirror101(2 * i + dx, W))];
        }
      }
      out[size_t(j)][size_t(i)] = floor_div(s + 128, 256);
    }
  }
  return out;
}

// 1D forward 5/3 LL only (we only need the low band for levels): pad to even by
// replication, extend symmetrically, lift.
std::vector<long> lowpass53(const std::vector<long>& in) {
  std::vector<long> x = in;
  if (x.size() % 2) {
    x.push_back(x.back());
  }
  const long n = long(x.size());
  auto X = [&](long i) {  // whole-sample symmetric extension
    if (i < 0) {
      i = -i;
    }
    if (i >= n) {
      i = 2 * (n - 1) - i;
    }
    return x[size_t(i)];
  };
  const long m = n / 2;
  auto D = [&](long k) {
    if (k < 0) {
      k = -1 - k;  // d[-1] = d[0] under symmetric extension
    }
    return X(2 * k + 1) - floor_div(X(2 * k) + X(2 * k + 2), 2);
  };
  std::vector<long> s(static_cast<size_t>(m));
  for (long k = 0; k < m; ++k) {
    s[size_t(k)] = X(2 * k) + floor_div(D(k - 1) + D(k) + 2, 4);
  }
  return s;
}

Grid cdf53_down(const Grid& g) {
  // rows first, keep low halves
  Grid rows;
  for (const auto& r : g) {
    rows.push_back(lowpass53(r));
  }
  if (rows.size() % 2) {
    rows.push_back(rows.back());  // pad height (replicate) before columns
  }
  const size_t w = rows[0].size(), h = rows.size() / 2;
  Grid out(h, std::vector<long>(w));
  for (size_t i = 0; i < w; ++i) {
    std::vector<long> col;
    for (const auto& r : rows) {
      col.push_back(r[i]);
    }
    const auto s = lowpass53(col);
    for (size_t j = 0; j < h; ++j) {
      out[j][i] = s[j];
    }
  }
  return out;
}

}  // namespace

std::vector<Image> pyramid(const Image& src,
                           int levels,
                           Transform t,
                           Downsampler d) {
  const int C = src.channels();
  std::vector<std::vector<Grid>> lv(1);
  for (int c = 0; c < C; ++c) {
    Grid g(size_t(src.height), std::vector<long>(size_t(src.width)));
    for (int y = 0; y < src.height; ++y) {
      for (int x = 0; x < src.width; ++x) {
        g[size_t(y)][size_t(x)] = src.at(x, y, c);
      }
    }
    lv[0].push_back(g);
  }
  for (int L = 1; L < levels; ++L) {
    std::vector<Grid> next;
    for (const Grid& g : lv.back()) {
      switch (t) {
        case Transform::Haar:
          next.push_back(haar_down(g));
          break;
        case Transform::Cdf53:
          next.push_back(cdf53_down(g));
          break;
        default:
          next.push_back(d == Downsampler::Gauss ? gauss_down(g) : box_down(g));
          break;
      }
    }
    lv.push_back(next);
  }
  std::vector<Image> out;
  for (const auto& chans : lv) {
    const int h = int(chans[0].size()), w = int(chans[0][0].size());
    Image img(w, h, src.format);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        for (int c = 0; c < C; ++c) {
          long v = chans[size_t(c)][size_t(y)][size_t(x)];
          v = v < 0 ? 0 : (v > 255 ? 255 : v);
          img.row(y)[size_t(x) * size_t(C) + size_t(c)] = uint8_t(v);
        }
      }
    }
    out.push_back(img);
  }
  return out;
}

Image synthetic_image(int w, int h, ColorFormat f, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> U(0, 1);
  const double fx = 0.01 + 0.05 * U(rng), fy = 0.01 + 0.05 * U(rng),
               ph = 6.28 * U(rng);
  const int cx = int(w * U(rng)), cy = int(h * U(rng)),
            rad = 1 + int(std::min(w, h) * 0.3 * U(rng));
  Image img(w, h, f);
  const int C = img.channels();
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (int c = 0; c < C; ++c) {
        double v = 128 + 70 * std::sin(fx * x + ph + c) * std::cos(fy * y - c);
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < rad * rad) {
          v = 230 - 40 * c;  // sharp edge
        }
        if (x % 37 < 2) {
          v = 20;  // thin lines
        }
        v += double(int(rng() % 9) - 4);  // noise
        img.row(y)[size_t(x) * size_t(C) + size_t(c)] =
            uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
      }
    }
  }
  return img;
}

}  // namespace ref
