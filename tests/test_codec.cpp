// End-to-end .prmd tests: crop matrix, fuzz, bounds, random access, format
// integrity.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <tuple>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/errors.h"
#include "prmd/internal/prmd_format.h"
#include "reference.h"
#include "test_util.h"

using namespace prmd;
using testutil::Config;

namespace {

struct Fixture {
  Image src;
  std::vector<Image> ref;  // reference pyramid
  std::string path;
  EncodeReport report;
};

// Encodes once per (config, size, format) and caches the result for all
// tests.
const Fixture& fixture(const Config& c,
                       int w,
                       int h,
                       ColorFormat f,
                       int levels = 5) {
  static std::map<std::string, Fixture> cache;
  const std::string key = std::string(c.name) + "_" + std::to_string(w) + "x" +
                          std::to_string(h) + "_" + info(f).name + "_" +
                          std::to_string(levels);
  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }

  Fixture fx;
  fx.src = ref::synthetic_image(w, h, f, unsigned(w * 13 + h));
  fx.ref = ref::pyramid(fx.src, levels, c.t, c.d);
  fx.path = testutil::temp_path(key + ".prmd").string();
  fx.report = encode_file(fx.src, fx.path, Codec::Prmd,
                          testutil::options_for(c, levels, f));
  return cache.emplace(key, std::move(fx)).first->second;
}

void expect_crop(CropDecoder& dec, const Fixture& fx, int L, const Rect& r) {
  const DecodeResult res = dec.decode(L, r);
  const Image exp = fx.ref[size_t(L)].crop(r);
  ASSERT_EQ(res.rect, r);
  ASSERT_EQ(res.image.width, r.w);
  ASSERT_EQ(res.image.height, r.h);
  ASSERT_EQ(res.image.format, exp.format);
  ASSERT_TRUE(res.image.pixels == exp.pixels)
      << dec.name() << " L" << L << " rect " << testutil::to_string(r);
}

// The named crop cases from the task, in level-L coordinates (tile = 256,
// cblk = 128).
std::vector<std::pair<std::string, Rect>> named_cases(int W, int H) {
  std::vector<std::pair<std::string, Rect>> v;
  auto add = [&](const char* n, Rect r) {
    const Rect c = intersect(r, Rect{0, 0, W, H});
    if (!c.empty()) {
      v.push_back({n, c});
    }
  };

  add("inside_one_block", {10, 20, 50, 40});
  add("inside_one_tile", {130, 5, 100, 200});
  add("across_2_tiles_h", {200, 30, 120, 60});
  add("across_2_tiles_v", {30, 200, 60, 120});
  add("across_4_tiles", {230, 230, 60, 60});
  add("edge_left", {0, H / 3, 40, 50});
  add("edge_right", {W - 40, H / 3, 40, 50});
  add("edge_top", {W / 3, 0, 50, 40});
  add("edge_bottom", {W / 3, H - 40, 50, 40});
  add("corner_tl", {0, 0, 33, 33});
  add("corner_tr", {W - 33, 0, 33, 33});
  add("corner_bl", {0, H - 33, 33, 33});
  add("corner_br", {W - 33, H - 33, 33, 33});
  add("full_level", {0, 0, W, H});
  add("one_pixel", {W / 2, H / 2, 1, 1});
  add("one_pixel_corner", {W - 1, H - 1, 1, 1});
  return v;
}

std::string size_param_name(const Config& c, std::pair<int, int> size) {
  return std::string(c.name) + "_" + std::to_string(size.first) + "x" +
         std::to_string(size.second);
}

}  // namespace

// ----------------------------------------------------------------------------
// Crop matrix

class CropTest : public ::testing::TestWithParam<
                     std::tuple<Config, ColorFormat, std::pair<int, int>>> {};

TEST_P(CropTest, NamedCasesAllLevels) {
  const auto& [cfg, fmt, size] = GetParam();
  const Fixture& fx = fixture(cfg, size.first, size.second, fmt);
  auto dec = open_decoder(fx.path);
  ASSERT_EQ(dec->num_levels(), 5);
  ASSERT_EQ(dec->color_format(), fmt);

  for (int L = 0; L < dec->num_levels(); ++L) {
    const LevelInfo li = dec->level_info(L);
    ASSERT_EQ(li.width, fx.ref[size_t(L)].width);
    ASSERT_EQ(li.height, fx.ref[size_t(L)].height);
    for (const auto& [name, r] : named_cases(li.width, li.height)) {
      SCOPED_TRACE(name);
      expect_crop(*dec, fx, L, r);
    }
  }
}

TEST_P(CropTest, RandomCropsFuzz) {
  const auto& [cfg, fmt, size] = GetParam();
  const Fixture& fx = fixture(cfg, size.first, size.second, fmt);
  auto dec = open_decoder(fx.path);
  std::mt19937 rng(77);
  const int iters = fmt == ColorFormat::Gray8 ? 400 : 120;

  for (int i = 0; i < iters; ++i) {
    const int L = int(rng() % unsigned(dec->num_levels()));
    const LevelInfo li = dec->level_info(L);
    expect_crop(*dec, fx, L,
                testutil::random_rect(rng, li.width, li.height, 300));
  }
}

INSTANTIATE_TEST_SUITE_P(
    Gray,
    CropTest,
    ::testing::Combine(::testing::ValuesIn(testutil::prmd_configs()),
                       ::testing::Values(ColorFormat::Gray8),
                       ::testing::Values(std::pair{1024, 1024},
                                         std::pair{1000, 777})),
    [](const auto& info) {
      return size_param_name(std::get<0>(info.param), std::get<2>(info.param));
    });

INSTANTIATE_TEST_SUITE_P(
    Rgb,
    CropTest,
    ::testing::Combine(::testing::ValuesIn(testutil::prmd_configs()),
                       ::testing::Values(ColorFormat::RGB8),
                       ::testing::Values(std::pair{512, 384},
                                         std::pair{301, 517})),
    [](const auto& info) {
      return size_param_name(std::get<0>(info.param), std::get<2>(info.param));
    });

TEST(Codec, TinyImagesAndManyLevels) {
  for (const auto& cfg : testutil::prmd_configs()) {
    for (auto [w, h] : {std::pair{1, 1}, std::pair{3, 2}, std::pair{17, 1}}) {
      const Fixture& fx = fixture(cfg, w, h, ColorFormat::Gray8, 6);
      auto dec = open_decoder(fx.path);
      for (int L = 0; L < 6; ++L) {
        const LevelInfo li = dec->level_info(L);
        expect_crop(*dec, fx, L, Rect{0, 0, li.width, li.height});
      }
    }
  }
}

TEST(Codec, NonDefaultTileAndBlockSizes) {
  const Config cfg = testutil::prmd_configs()[1];  // cdf53
  const Image src = ref::synthetic_image(300, 200, ColorFormat::Gray8, 5);
  const auto ref = ref::pyramid(src, 4, cfg.t, cfg.d);

  for (auto [tile, cblk] :
       {std::pair{64, 16}, std::pair{256, 256}, std::pair{512, 64}}) {
    const auto path = testutil::temp_path("sizes_" + std::to_string(tile) +
                                          "_" + std::to_string(cblk) + ".prmd");
    encode_file(src, path.string(), Codec::Prmd,
                testutil::options_for(cfg, 4, ColorFormat::Gray8, tile, cblk));
    auto dec = open_decoder(path.string());

    std::mt19937 rng(3);
    for (int i = 0; i < 100; ++i) {
      const int L = int(rng() % 4u);
      const LevelInfo li = dec->level_info(L);
      // No size cap: any rect inside the level.
      const Rect r = testutil::random_rect(rng, li.width, li.height,
                                           std::max(li.width, li.height));
      const auto res = dec->decode(L, r);
      ASSERT_EQ(res.image.pixels, ref[size_t(L)].crop(r).pixels)
          << tile << "/" << cblk;
    }
  }
}

TEST(Codec, EncoderConvertsColorFormat) {
  const Config cfg = testutil::prmd_configs()[0];
  const Image rgb = ref::synthetic_image(64, 64, ColorFormat::RGB8, 1);
  const auto path = testutil::temp_path("convert.prmd").string();
  encode_file(rgb, path, Codec::Prmd,
              testutil::options_for(cfg, 3, ColorFormat::Gray8));

  auto dec = open_decoder(path);
  EXPECT_EQ(dec->color_format(), ColorFormat::Gray8);
  EXPECT_EQ(dec->decode(0, {0, 0, 64, 64}).image.pixels,
            convert(rgb, ColorFormat::Gray8).pixels);
}

// ----------------------------------------------------------------------------
// Bounds

TEST(Bounds, StrictRejectsAndClampedIntersects) {
  const Fixture& fx =
      fixture(testutil::prmd_configs()[0], 1000, 777, ColorFormat::Gray8);
  auto dec = open_decoder(fx.path);
  const LevelInfo l2 = dec->level_info(2);  // 250 x 195

  EXPECT_THROW(dec->decode(-1, {0, 0, 1, 1}), std::out_of_range);
  EXPECT_THROW(dec->decode(5, {0, 0, 1, 1}), std::out_of_range);
  EXPECT_THROW(dec->decode(2, {-1, 0, 10, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode(2, {0, -1, 10, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode(2, {0, 0, 0, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode(2, {0, 0, 10, -3}), std::out_of_range);
  EXPECT_THROW(dec->decode(2, {l2.width - 5, 0, 6, 1}), std::out_of_range);
  EXPECT_THROW(dec->decode(2, {0, l2.height, 1, 1}), std::out_of_range);

  const auto res = dec->decode_clamped(2, {l2.width - 30, -20, 100, 50});
  EXPECT_EQ(res.rect, (Rect{l2.width - 30, 0, 30, 30}));
  EXPECT_EQ(res.image.pixels, fx.ref[2].crop(res.rect).pixels);
  EXPECT_THROW(dec->decode_clamped(2, {l2.width, 0, 10, 10}),
               std::out_of_range);
  EXPECT_THROW(dec->decode_clamped(2, {-50, -50, 10, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode_clamped(7, {0, 0, 10, 10}), std::out_of_range);
}

// ----------------------------------------------------------------------------
// Random access

namespace {

// Analytic set of block ids a crop needs, derived independently from the
// decoder: walks the same dependency rule (DESIGN.md §5/§6) with plain
// interval arithmetic.
std::set<uint32_t> expected_blocks(const Config& c,
                                   int W,
                                   int H,
                                   int levels,
                                   int L,
                                   Rect r) {
  using namespace prmd::internal;
  const auto geom = make_geometry(W, H, levels, 256, 128, c.t);
  std::set<uint32_t> ids;

  auto add_blocks = [&](int lv, const Rect& need, int span) {
    const auto& g = geom[size_t(lv)];
    for (int by = need.y / span;
         by <= std::min((need.bottom() - 1) / span, g.blocks_y - 1); ++by) {
      for (int bx = need.x / span;
           bx <= std::min((need.right() - 1) / span, g.blocks_x - 1); ++bx) {
        ids.insert(block_id(g, bx, by));
      }
    }
  };

  for (int lv = L;; ++lv) {
    const auto& g = geom[size_t(lv)];
    const int cw = (g.w + 1) / 2;
    const int ch = (g.h + 1) / 2;
    if (g.base) {
      add_blocks(lv, r, 128);
      break;
    }

    // Coarse samples n-1 .. n+1 around every fine sample, clipped.
    auto widened_half = [&]() {
      const int x0 = std::max(r.x / 2 - 1, 0);
      const int x1 = std::min((r.right() - 1) / 2 + 1, cw - 1);
      const int y0 = std::max(r.y / 2 - 1, 0);
      const int y1 = std::min((r.bottom() - 1) / 2 + 1, ch - 1);
      return Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    };

    Rect band;
    Rect coarse;
    if (c.t == Transform::Haar) {
      band = coarse = Rect{r.x / 2, r.y / 2, (r.right() + 1) / 2 - r.x / 2,
                           (r.bottom() + 1) / 2 - r.y / 2};
    } else if (c.t == Transform::Cdf53) {
      band = coarse = widened_half();
    } else {  // laplacian
      band = r;
      coarse = widened_half();
    }
    add_blocks(lv, band, c.t == Transform::Laplacian ? 128 : 64);
    r = coarse;
  }
  return ids;
}

struct IndexInfo {
  uint64_t data_offset;
  std::vector<uint32_t> offsets;
};

IndexInfo read_index(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> head(prmd::internal::kHeaderSize);
  f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  const auto h = prmd::internal::parse_header(head.data(), head.size());

  std::vector<uint8_t> idx(prmd::internal::index_bytes(h));
  f.read(reinterpret_cast<char*>(idx.data()), std::streamsize(idx.size()));

  std::vector<prmd::internal::LevelEntry> lv;
  IndexInfo ii{h.data_offset, {}};
  prmd::internal::parse_index(h, idx.data(), idx.size(), lv, ii.offsets);
  return ii;
}

// Overwrites the payload of block `id` in the file image `bytes` with
// garbage.
void corrupt_block(std::vector<char>& bytes,
                   const IndexInfo& ii,
                   uint32_t id,
                   std::mt19937& garbage) {
  for (uint64_t o = ii.offsets[id]; o < ii.offsets[id + 1]; ++o) {
    bytes[size_t(ii.data_offset + o)] = char(garbage());
  }
}

}  // namespace

class RandomAccessTest : public ::testing::TestWithParam<Config> {};

// (a) The decoder reads exactly the expected blocks: same count, and
//     bytes_read equals the sum of those payloads.
// (b) Overwriting every payload outside that set with garbage doesn't change
//     the crop.
TEST_P(RandomAccessTest, ReadsOnlyExpectedBlocks) {
  const Config& cfg = GetParam();
  const int W = 1024;
  const int H = 1024;
  const Fixture& fx = fixture(cfg, W, H, ColorFormat::Gray8);
  const IndexInfo ii = read_index(fx.path);
  std::mt19937 rng(11);

  for (int it = 0; it < 12; ++it) {
    const int L = it % 3;
    const LevelInfo li{W >> L, H >> L};
    const int w = std::min(li.width, 64 + int(rng() % 200u));
    const int h = std::min(li.height, 64 + int(rng() % 200u));
    const int x = int(rng() % unsigned(li.width - w + 1));
    const int y = int(rng() % unsigned(li.height - h + 1));
    const Rect r{x, y, w, h};

    const auto ids = expected_blocks(cfg, W, H, 5, L, r);
    uint64_t exp_bytes = 0;
    for (uint32_t id : ids) {
      exp_bytes += ii.offsets[id + 1] - ii.offsets[id];
    }

    // (a)
    auto dec = open_decoder(fx.path);
    const auto res = dec->decode(L, r);
    EXPECT_EQ(size_t(res.stats.blocks_decoded), ids.size())
        << cfg.name << " it " << it;
    EXPECT_EQ(res.stats.bytes_read, exp_bytes) << cfg.name << " it " << it;
    EXPECT_LT(res.stats.bytes_read, ii.offsets.back() / 2)
        << "a crop must not read most of the file";
    EXPECT_EQ(dec->open_bytes_read(), ii.data_offset);

    // (b) Corrupt everything else.
    std::vector<char> bytes = testutil::read_file(fx.path);
    std::mt19937 garbage(5);
    for (uint32_t id = 0; id + 1 < ii.offsets.size(); ++id) {
      if (!ids.count(id)) {
        corrupt_block(bytes, ii, id, garbage);
      }
    }
    const auto corrupt =
        testutil::temp_path(std::string("corrupt_") + cfg.name + ".prmd")
            .string();
    testutil::write_file(corrupt, bytes);

    auto dec2 = open_decoder(corrupt);
    EXPECT_EQ(dec2->decode(L, r).image.pixels, fx.ref[size_t(L)].crop(r).pixels)
        << cfg.name << " it " << it;
  }
}

// Negative control for (b): corrupting a block that *is* in the expected set
// must change the result (or fail to decode). Otherwise the corruption test
// above would prove nothing.
TEST_P(RandomAccessTest, CorruptingANeededBlockIsDetected) {
  const Config& cfg = GetParam();
  const Fixture& fx = fixture(cfg, 1024, 1024, ColorFormat::Gray8);
  const IndexInfo ii = read_index(fx.path);
  const Rect r{300, 300, 100, 100};
  const auto ids = expected_blocks(cfg, 1024, 1024, 5, 0, r);

  std::vector<char> bytes = testutil::read_file(fx.path);
  std::mt19937 garbage(9);
  corrupt_block(bytes, ii, *ids.begin(), garbage);
  const auto p =
      testutil::temp_path(std::string("victim_") + cfg.name + ".prmd").string();
  testutil::write_file(p, bytes);

  auto dec = open_decoder(p);
  bool differs = false;
  try {
    differs = dec->decode(0, r).image.pixels != fx.ref[0].crop(r).pixels;
  } catch (const std::exception&) {
    differs = true;
  }
  EXPECT_TRUE(differs) << cfg.name;
}

INSTANTIATE_TEST_SUITE_P(All,
                         RandomAccessTest,
                         ::testing::ValuesIn(testutil::prmd_configs()),
                         [](const auto& info) {
                           return std::string(info.param.name);
                         });

TEST(RandomAccess, CacheServesRepeatedCrops) {
  const Fixture& fx =
      fixture(testutil::prmd_configs()[0], 1024, 1024, ColorFormat::Gray8);
  DecoderOptions o;
  o.cache_bytes = 64 << 20;
  auto dec = open_decoder(fx.path, o);

  const auto a = dec->decode(0, {300, 300, 256, 256});
  const auto b = dec->decode(0, {300, 300, 256, 256});
  EXPECT_GT(a.stats.blocks_decoded, 0);
  EXPECT_EQ(b.stats.blocks_decoded, 0);
  EXPECT_EQ(b.stats.bytes_read, 0u);
  EXPECT_EQ(b.stats.cache_hits, a.stats.blocks_decoded);
  EXPECT_EQ(a.image.pixels, b.image.pixels);
}

TEST(RandomAccess, NaiveNeedsNoAncestors) {
  const Fixture& fx =
      fixture(testutil::prmd_configs()[4], 1024, 1024, ColorFormat::Gray8);
  auto dec = open_decoder(fx.path);
  const auto res = dec->decode(0, {10, 10, 100, 100});
  EXPECT_EQ(res.stats.blocks_decoded, 1);
  EXPECT_EQ(res.stats.tiles_touched, 1);
}

// ----------------------------------------------------------------------------
// Format integrity

TEST(Format, RejectsCorruptHeadersAndIndex) {
  const Fixture& fx =
      fixture(testutil::prmd_configs()[0], 1024, 1024, ColorFormat::Gray8);
  const std::vector<char> good = testutil::read_file(fx.path);
  auto try_open = [&](const std::vector<char>& bytes) {
    const auto p = testutil::temp_path("bad.prmd").string();
    testutil::write_file(p, bytes);
    return open_decoder(p);
  };
  // `good` with byte `at` replaced by `value`.
  auto with_byte = [&](size_t at, char value) {
    std::vector<char> bad = good;
    bad[at] = value;
    return bad;
  };

  EXPECT_NO_THROW(try_open(good));
  EXPECT_THROW(try_open(with_byte(0, 'X')), FormatError);  // magic
  EXPECT_THROW(try_open(with_byte(4, 9)), FormatError);    // version
  EXPECT_THROW(try_open(with_byte(17, 7)), FormatError);   // color id
  EXPECT_THROW(try_open(with_byte(40, 9)), FormatError);   // entropy id

  const size_t index_byte = internal::kHeaderSize + 3;
  EXPECT_THROW(try_open(with_byte(index_byte, char(good[index_byte] ^ 1))),
               FormatError);  // index CRC

  std::vector<char> truncated(good.begin(), good.end() - 1);
  EXPECT_THROW(try_open(truncated), FormatError);  // truncated data
  truncated.assign(good.begin(), good.begin() + 30);
  EXPECT_THROW(try_open(truncated), FormatError);  // truncated header
}

TEST(Format, EncoderValidatesOptions) {
  const Image img(16, 16);
  const auto p = testutil::temp_path("opts.prmd").string();
  auto expect_invalid = [&](const EncodeOptions& o) {
    EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
  };

  EncodeOptions o;
  o.levels = 0;
  expect_invalid(o);

  o = {};
  o.cblk = 100;
  o.tile = 256;
  expect_invalid(o);

  o = {};
  o.quant = -1;
  expect_invalid(o);

  o = {};
  o.quant = 4;
  o.transform = Transform::Independent;  // naive is lossless only
  expect_invalid(o);

  o = {};
  o.entropy = Entropy::Range;
  o.entropy_level = 5;  // range takes no level
  expect_invalid(o);

  o = {};
  o.entropy_level = 23;  // zstd: 1..22
  expect_invalid(o);
}

TEST(Format, DefaultEntropyCoderIsZstd) {
  EXPECT_EQ(EncodeOptions{}.entropy, Entropy::Zstd);
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  const auto p = testutil::temp_path("default_coder.prmd").string();

  encode_file(img, p, Codec::Prmd, EncodeOptions{});
  EXPECT_EQ(open_decoder(p)->name(), "prmd/haar");

  EncodeOptions r;
  r.entropy = Entropy::Range;
  encode_file(img, p, Codec::Prmd, r);
  EXPECT_EQ(open_decoder(p)->name(), "prmd/haar+range");
}

TEST(Format, HaarAndCdf53AreSmallerThanNaive) {
  const auto& cfgs = testutil::prmd_configs();
  const auto haar =
      fixture(cfgs[0], 1024, 1024, ColorFormat::Gray8).report.file_bytes;
  const auto cdf =
      fixture(cfgs[1], 1024, 1024, ColorFormat::Gray8).report.file_bytes;
  const auto naive =
      fixture(cfgs[4], 1024, 1024, ColorFormat::Gray8).report.file_bytes;
  EXPECT_LT(haar, naive);
  EXPECT_LT(cdf, naive);
}
