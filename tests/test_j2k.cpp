// JPEG2000 backend: same crop matrix as .prmd, against J2K's own full-level decode
// (its levels are 5/3 LL bands, not the 2×2 mean). Level 0 is lossless, so it is also
// compared with the source.

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <tuple>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "reference.h"
#include "test_util.h"

using namespace prmd;

namespace {

struct J2kFixture {
  Image src;
  std::string path;
  std::vector<Image> full;  // full decode of every level (the reference)
};

const J2kFixture& j2k_fixture(int w, int h, ColorFormat f, int tile) {
  static std::map<std::string, J2kFixture> cache;
  const std::string key = std::to_string(w) + "x" + std::to_string(h) + "_" + info(f).name + "_t" + std::to_string(tile);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  J2kFixture fx;
  fx.src = ref::synthetic_image(w, h, f, unsigned(w + 7 * h));
  fx.path = testutil::temp_path("j2k_" + key + ".j2k").string();
  EncodeOptions o;
  o.levels = 5;
  o.color = f;
  o.j2k_tile = tile;
  encode_file(fx.src, fx.path, Codec::J2K, o);
  auto dec = open_decoder(fx.path);
  for (int L = 0; L < dec->num_levels(); ++L) {
    const LevelInfo li = dec->level_info(L);
    fx.full.push_back(dec->decode(L, {0, 0, li.width, li.height}).image);
  }
  return cache.emplace(key, std::move(fx)).first->second;
}

}  // namespace

class J2kTest : public ::testing::TestWithParam<std::tuple<ColorFormat, int>> {};

TEST_P(J2kTest, LosslessAndCropsMatchFullDecode) {
  const auto [fmt, tile] = GetParam();
  const int w = fmt == ColorFormat::Gray8 ? 1000 : 512, h = fmt == ColorFormat::Gray8 ? 777 : 384;
  const J2kFixture& fx = j2k_fixture(w, h, fmt, tile);
  auto dec = open_decoder(fx.path);
  EXPECT_EQ(dec->num_levels(), 5);
  EXPECT_EQ(dec->color_format(), fmt);
  EXPECT_EQ(dec->name(), "j2k-t" + std::to_string(tile));
  ASSERT_EQ(fx.full[0].pixels, fx.src.pixels) << "J2K level 0 must be lossless";
  for (int L = 0; L < 5; ++L) {
    EXPECT_EQ(fx.full[size_t(L)].width, (w + (1 << L) - 1) >> L);
    EXPECT_EQ(fx.full[size_t(L)].height, (h + (1 << L) - 1) >> L);
  }
  std::mt19937 rng(5);
  for (int i = 0; i < 150; ++i) {
    const int L = int(rng() % 5u);
    const LevelInfo li = dec->level_info(L);
    const int cw = 1 + int(rng() % unsigned(std::min(li.width, 300)));
    const int ch = 1 + int(rng() % unsigned(std::min(li.height, 300)));
    const Rect r{int(rng() % unsigned(li.width - cw + 1)), int(rng() % unsigned(li.height - ch + 1)), cw, ch};
    const auto res = dec->decode(L, r);
    ASSERT_EQ(res.image.pixels, fx.full[size_t(L)].crop(r).pixels)
        << "L" << L << " {" << r.x << "," << r.y << "," << r.w << "," << r.h << "}";
  }
  // Corners and edges at every level.
  for (int L = 0; L < 5; ++L) {
    const LevelInfo li = dec->level_info(L);
    for (const Rect& r : {Rect{0, 0, 1, 1}, Rect{li.width - 1, li.height - 1, 1, 1},
                          Rect{li.width - std::min(li.width, 40), 0, std::min(li.width, 40), std::min(li.height, 30)}})
      ASSERT_EQ(dec->decode(L, r).image.pixels, fx.full[size_t(L)].crop(r).pixels);
  }
}

INSTANTIATE_TEST_SUITE_P(All, J2kTest,
                         ::testing::Combine(::testing::Values(ColorFormat::Gray8, ColorFormat::RGB8),
                                            ::testing::Values(256, 1024)),
                         [](const auto& info) {
                           return std::string(prmd::info(std::get<0>(info.param)).name) + "_t" +
                                  std::to_string(std::get<1>(info.param));
                         });

TEST(J2k, TlmLetsSingleTileCropsSkipData) {
  const J2kFixture& fx = j2k_fixture(1000, 777, ColorFormat::Gray8, 256);
  auto dec = open_decoder(fx.path);
  const auto res = dec->decode(0, {300, 300, 100, 100});  // inside one 256 tile
  EXPECT_EQ(res.stats.tiles_touched, 1);
  EXPECT_LT(res.stats.bytes_read * 4, dec->file_bytes()) << "a one-tile crop must not read most of the file";
}

TEST(J2k, Bounds) {
  const J2kFixture& fx = j2k_fixture(1000, 777, ColorFormat::Gray8, 256);
  auto dec = open_decoder(fx.path);
  EXPECT_THROW(dec->decode(0, {990, 0, 20, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode(5, {0, 0, 1, 1}), std::out_of_range);
  const auto res = dec->decode_clamped(1, {480, 380, 100, 100});
  EXPECT_EQ(res.rect, (Rect{480, 380, 20, 9}));
}
