#include <gtest/gtest.h>

#include <random>
#include <tuple>

#include "prmd/internal/transform.h"
#include "prmd/pyramid.h"
#include "reference.h"

using namespace prmd;
using namespace prmd::internal;

namespace {

struct Mode {
  Transform t;
  Downsampler d;
  const char* name;
};
void PrintTo(const Mode& m, std::ostream* os) {
  *os << m.name;
}

const Mode kModes[] = {
    {Transform::Haar, Downsampler::Box, "haar"},
    {Transform::Cdf53, Downsampler::Box, "cdf53"},
    {Transform::Laplacian, Downsampler::Box, "laplacian_box"},
    {Transform::Laplacian, Downsampler::Gauss, "laplacian_gauss"},
    {Transform::Independent, Downsampler::Box, "naive_box"},
};
const Mode kXformModes[] = {kModes[0], kModes[1], kModes[2], kModes[3]};

Region random_plane(int w, int h, int lo, int hi, unsigned seed) {
  Region r(Rect{0, 0, w, h});
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> U(lo, hi);
  for (auto& v : r.v) {
    v = U(rng);
  }
  return r;
}

// Overwrites every sample of `plane` outside `keep` with value(x, y).
template <class ValueFn>
void scribble_outside(Region& plane, const Rect& keep, ValueFn value) {
  for (int y = 0; y < plane.rect.h; ++y) {
    for (int x = 0; x < plane.rect.w; ++x) {
      if (!intersect(keep, Rect{x, y, 1, 1}).w) {
        plane.at(x, y) = value(x, y);
      }
    }
  }
}

}  // namespace

// ----------------------------------------------------------------------------
// Level definitions

class PyramidVsReference
    : public ::testing::TestWithParam<std::tuple<Mode, ColorFormat>> {};

TEST_P(PyramidVsReference, MatchesIndependentReference) {
  const auto [mode, fmt] = GetParam();
  for (auto [w, h] : {std::pair{64, 64}, std::pair{101, 77}, std::pair{1, 9},
                      std::pair{2, 3}, std::pair{257, 130}}) {
    const Image src = ref::synthetic_image(w, h, fmt, unsigned(w * 31 + h));
    const auto got = build_pyramid(src, 5, mode.t, mode.d);
    const auto exp = ref::pyramid(src, 5, mode.t, mode.d);
    ASSERT_EQ(got.size(), exp.size());
    for (size_t L = 0; L < got.size(); ++L) {
      ASSERT_EQ(got[L].width, exp[L].width) << mode.name << " L" << L;
      ASSERT_EQ(got[L].height, exp[L].height) << mode.name << " L" << L;
      EXPECT_EQ(got[L].width, (w + (1 << L) - 1) >> L);
      ASSERT_EQ(got[L].pixels, exp[L].pixels)
          << mode.name << " " << w << "x" << h << " L" << L;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    All,
    PyramidVsReference,
    ::testing::Combine(::testing::ValuesIn(kModes),
                       ::testing::Values(ColorFormat::Gray8,
                                         ColorFormat::RGB8)),
    [](const auto& info) {
      return std::string(std::get<0>(info.param).name) + "_" +
             prmd::info(std::get<1>(info.param)).name;
    });

TEST(Pyramid, HaarIsTheTwoByTwoMean) {
  Image img(2, 2);
  img.pixels = {100, 104, 98, 110};
  EXPECT_EQ(
      build_pyramid(img, 2, Transform::Haar, Downsampler::Box)[1].at(0, 0),
      103);
}

// ----------------------------------------------------------------------------
// Transforms

class TransformTest : public ::testing::TestWithParam<Mode> {};

TEST_P(TransformTest, ForwardInverseIdentityWholePlane) {
  const auto xf = make_transform(GetParam().t, GetParam().d);
  for (auto [w, h] : {std::pair{16, 16}, std::pair{17, 9}, std::pair{1, 1},
                      std::pair{2, 1}, std::pair{1, 5}, std::pair{63, 64}}) {
    // Wide value range: Cdf53 levels > 0 carry values outside [0, 255].
    const Region fine = random_plane(w, h, -300, 600, unsigned(w * 7 + h));
    Region coarse;
    std::vector<Region> bands;
    xf->forward(fine, coarse, bands);
    const Dims fd{w, h};
    EXPECT_EQ(coarse.rect.w, (w + 1) / 2);
    EXPECT_EQ(coarse.rect.h, (h + 1) / 2);
    ASSERT_EQ(int(bands.size()), xf->num_bands());
    for (const auto& b : bands) {
      EXPECT_EQ(b.rect.w, xf->band_size(fd).w);
      EXPECT_EQ(b.rect.h, xf->band_size(fd).h);
    }
    Region out;
    xf->inverse(coarse, bands, fine.rect, fd, out);
    ASSERT_EQ(out.v, fine.v) << GetParam().name << " " << w << "x" << h;
  }
}

// Region inverse equals the full inverse, and depends only on samples inside
// *_needed: everything outside those rects is overwritten with garbage first.
TEST_P(TransformTest, RegionInverseUsesOnlyNeededSamples) {
  const auto xf = make_transform(GetParam().t, GetParam().d);
  std::mt19937 rng(42);
  for (auto [w, h] : {std::pair{64, 48}, std::pair{37, 29}}) {
    const Region fine = random_plane(w, h, 0, 255, unsigned(w + h));
    Region coarse;
    std::vector<Region> bands;
    xf->forward(fine, coarse, bands);
    const Dims fd{w, h};
    for (int it = 0; it < 300; ++it) {
      const int x = int(rng() % unsigned(w));
      const int y = int(rng() % unsigned(h));
      const int rw = 1 + int(rng() % unsigned(w - x));
      const int rh = 1 + int(rng() % unsigned(h - y));
      const Rect r{x, y, rw, rh};

      Region c2 = coarse;
      scribble_outside(c2, xf->coarse_needed(r, fd),
                       [](int xx, int) { return 0x5EED + xx; });
      std::vector<Region> b2 = bands;
      for (auto& b : b2) {
        scribble_outside(b, xf->band_needed(r, fd),
                         [](int, int yy) { return -777 - yy; });
      }

      Region out;
      xf->inverse(c2, b2, r, fd, out);
      ASSERT_EQ(out.rect, r);
      for (int yy = r.y; yy < r.bottom(); ++yy) {
        for (int xx = r.x; xx < r.right(); ++xx) {
          ASSERT_EQ(out.at(xx, yy), fine.at(xx, yy))
              << GetParam().name << " rect " << r.x << "," << r.y << " " << r.w
              << "x" << r.h;
        }
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(All,
                         TransformTest,
                         ::testing::ValuesIn(kXformModes),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

TEST(Transform, IndependentHasNoLevelTransform) {
  EXPECT_THROW(make_transform(Transform::Independent, Downsampler::Box),
               std::invalid_argument);
}
