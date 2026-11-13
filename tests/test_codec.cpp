// End-to-end .prmd tests: crop matrix, fuzz, bounds, random access, format integrity.

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

// Encodes once per (config, size, format) and caches the result for all tests.
const Fixture& fixture(const Config& c, int w, int h, ColorFormat f, int levels = 5) {
  static std::map<std::string, Fixture> cache;
  const std::string key = std::string(c.name) + "_" + std::to_string(w) + "x" + std::to_string(h) + "_" +
                          info(f).name + "_" + std::to_string(levels);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  Fixture fx;
  fx.src = ref::synthetic_image(w, h, f, unsigned(w * 13 + h));
  fx.ref = ref::pyramid(fx.src, levels, c.t, c.d);
  fx.path = testutil::temp_path(key + ".prmd").string();
  fx.report = encode_file(fx.src, fx.path, Codec::Prmd, testutil::options_for(c, levels, f));
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
      << dec.name() << " L" << L << " rect {" << r.x << "," << r.y << "," << r.w << "," << r.h << "}";
}

// The named crop cases from the task, in level-L coordinates (tile = 256, cblk = 128).
std::vector<std::pair<std::string, Rect>> named_cases(int W, int H) {
  std::vector<std::pair<std::string, Rect>> v;
  auto add = [&](const char* n, Rect r) {
    const Rect c = intersect(r, Rect{0, 0, W, H});
    if (!c.empty()) v.push_back({n, c});
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

}  // namespace

// ------------------------------------------------------------------ crop matrix

class CropTest : public ::testing::TestWithParam<std::tuple<Config, ColorFormat, std::pair<int, int>>> {};

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
    const int w = 1 + int(rng() % unsigned(std::min(li.width, 300)));
    const int h = 1 + int(rng() % unsigned(std::min(li.height, 300)));
    const int x = int(rng() % unsigned(li.width - w + 1)), y = int(rng() % unsigned(li.height - h + 1));
    expect_crop(*dec, fx, L, Rect{x, y, w, h});
  }
}

INSTANTIATE_TEST_SUITE_P(
    Gray, CropTest,
    ::testing::Combine(::testing::ValuesIn(testutil::prmd_configs()), ::testing::Values(ColorFormat::Gray8),
                       ::testing::Values(std::pair{1024, 1024}, std::pair{1000, 777})),
    [](const auto& info) {
      const auto& p = info.param;
      return std::string(std::get<0>(p).name) + "_" + std::to_string(std::get<2>(p).first) + "x" +
             std::to_string(std::get<2>(p).second);
    });

INSTANTIATE_TEST_SUITE_P(
    Rgb, CropTest,
    ::testing::Combine(::testing::ValuesIn(testutil::prmd_configs()), ::testing::Values(ColorFormat::RGB8),
                       ::testing::Values(std::pair{512, 384}, std::pair{301, 517})),
    [](const auto& info) {
      const auto& p = info.param;
      return std::string(std::get<0>(p).name) + "_" + std::to_string(std::get<2>(p).first) + "x" +
             std::to_string(std::get<2>(p).second);
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
  for (auto [tile, cblk] : {std::pair{64, 16}, std::pair{256, 256}, std::pair{512, 64}}) {
    const auto path = testutil::temp_path("sizes_" + std::to_string(tile) + "_" + std::to_string(cblk) + ".prmd");
    encode_file(src, path.string(), Codec::Prmd, testutil::options_for(cfg, 4, ColorFormat::Gray8, tile, cblk));
    auto dec = open_decoder(path.string());
    std::mt19937 rng(3);
    for (int i = 0; i < 100; ++i) {
      const int L = int(rng() % 4u);
      const LevelInfo li = dec->level_info(L);
      const int w = 1 + int(rng() % unsigned(li.width)), h = 1 + int(rng() % unsigned(li.height));
      const int x = int(rng() % unsigned(li.width - w + 1)), y = int(rng() % unsigned(li.height - h + 1));
      const auto res = dec->decode(L, {x, y, w, h});
      ASSERT_EQ(res.image.pixels, ref[size_t(L)].crop({x, y, w, h}).pixels) << tile << "/" << cblk;
    }
  }
}

TEST(Codec, EncoderConvertsColorFormat) {
  const Config cfg = testutil::prmd_configs()[0];
  const Image rgb = ref::synthetic_image(64, 64, ColorFormat::RGB8, 1);
  const auto path = testutil::temp_path("convert.prmd").string();
  encode_file(rgb, path, Codec::Prmd, testutil::options_for(cfg, 3, ColorFormat::Gray8));
  auto dec = open_decoder(path);
  EXPECT_EQ(dec->color_format(), ColorFormat::Gray8);
  EXPECT_EQ(dec->decode(0, {0, 0, 64, 64}).image.pixels, convert(rgb, ColorFormat::Gray8).pixels);
}

// ------------------------------------------------------------------ bounds

TEST(Bounds, StrictRejectsAndClampedIntersects) {
  const Fixture& fx = fixture(testutil::prmd_configs()[0], 1000, 777, ColorFormat::Gray8);
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
  EXPECT_THROW(dec->decode_clamped(2, {l2.width, 0, 10, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode_clamped(2, {-50, -50, 10, 10}), std::out_of_range);
  EXPECT_THROW(dec->decode_clamped(7, {0, 0, 10, 10}), std::out_of_range);
}

// ------------------------------------------------------------------ random access

namespace {

// Analytic set of block ids a crop needs, derived independently from the decoder: walks the
// same dependency rule (DESIGN.md §5/§6) with plain interval arithmetic.
std::set<uint32_t> expected_blocks(const Config& c, int W, int H, int levels, int L, Rect r) {
  using namespace prmd::internal;
  const auto geom = make_geometry(W, H, levels, 256, 128, c.t);
  std::set<uint32_t> ids;
  auto add_blocks = [&](int lv, const Rect& need, int span) {
    const auto& g = geom[size_t(lv)];
    for (int by = need.y / span; by <= std::min((need.bottom() - 1) / span, g.blocks_y - 1); ++by)
      for (int bx = need.x / span; bx <= std::min((need.right() - 1) / span, g.blocks_x - 1); ++bx)
        ids.insert(block_id(g, bx, by));
  };
  for (int lv = L;; ++lv) {
    const auto& g = geom[size_t(lv)];
    const int cw = (g.w + 1) / 2, ch = (g.h + 1) / 2;
    if (g.base) {
      add_blocks(lv, r, 128);
      break;
    }
    Rect band, coarse;
    if (c.t == Transform::Haar) {
      band = coarse = Rect{r.x / 2, r.y / 2, (r.right() + 1) / 2 - r.x / 2, (r.bottom() + 1) / 2 - r.y / 2};
    } else if (c.t == Transform::Cdf53) {
      const int x0 = std::max(r.x / 2 - 1, 0), x1 = std::min((r.right() - 1) / 2 + 1, cw - 1);
      const int y0 = std::max(r.y / 2 - 1, 0), y1 = std::min((r.bottom() - 1) / 2 + 1, ch - 1);
      band = coarse = Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    } else {  // laplacian
      band = r;
      const int x0 = std::max(r.x / 2 - 1, 0), x1 = std::min((r.right() - 1) / 2 + 1, cw - 1);
      const int y0 = std::max(r.y / 2 - 1, 0), y1 = std::min((r.bottom() - 1) / 2 + 1, ch - 1);
      coarse = Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
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

}  // namespace

class RandomAccessTest : public ::testing::TestWithParam<Config> {};

// (a) The decoder reads exactly the expected blocks: same count, and bytes_read equals the
//     sum of those payloads.
// (b) Overwriting every payload outside that set with garbage doesn't change the crop.
TEST_P(RandomAccessTest, ReadsOnlyExpectedBlocks) {
  const Config& cfg = GetParam();
  const int W = 1024, H = 1024;
  const Fixture& fx = fixture(cfg, W, H, ColorFormat::Gray8);
  const IndexInfo ii = read_index(fx.path);
  std::mt19937 rng(11);
  for (int it = 0; it < 12; ++it) {
    const int L = it % 3;
    const LevelInfo li{W >> L, H >> L};
    const int w = std::min(li.width, 64 + int(rng() % 200u)), h = std::min(li.height, 64 + int(rng() % 200u));
    const Rect r{int(rng() % unsigned(li.width - w + 1)), int(rng() % unsigned(li.height - h + 1)), w, h};
    const auto ids = expected_blocks(cfg, W, H, 5, L, r);
    uint64_t exp_bytes = 0;
    for (uint32_t id : ids) exp_bytes += ii.offsets[id + 1] - ii.offsets[id];

    auto dec = open_decoder(fx.path);
    const auto res = dec->decode(L, r);
    EXPECT_EQ(size_t(res.stats.blocks_decoded), ids.size()) << cfg.name << " it " << it;
    EXPECT_EQ(res.stats.bytes_read, exp_bytes) << cfg.name << " it " << it;
    EXPECT_LT(res.stats.bytes_read, ii.offsets.back() / 2) << "a crop must not read most of the file";
    EXPECT_EQ(dec->open_bytes_read(), ii.data_offset);

    // (b) corrupt everything else
    std::vector<char> bytes;
    {
      std::ifstream f(fx.path, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::mt19937 garbage(5);
    for (uint32_t id = 0; id + 1 < ii.offsets.size(); ++id) {
      if (ids.count(id)) continue;
      for (uint64_t o = ii.offsets[id]; o < ii.offsets[id + 1]; ++o) bytes[size_t(ii.data_offset + o)] = char(garbage());
    }
    const auto corrupt = testutil::temp_path(std::string("corrupt_") + cfg.name + ".prmd").string();
    std::ofstream(corrupt, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    auto dec2 = open_decoder(corrupt);
    EXPECT_EQ(dec2->decode(L, r).image.pixels, fx.ref[size_t(L)].crop(r).pixels) << cfg.name << " it " << it;
  }
}

// Negative control for (b): corrupting a block that *is* in the expected set must change the
// result (or fail to decode). Otherwise the corruption test above would prove nothing.
TEST_P(RandomAccessTest, CorruptingANeededBlockIsDetected) {
  const Config& cfg = GetParam();
  const Fixture& fx = fixture(cfg, 1024, 1024, ColorFormat::Gray8);
  const IndexInfo ii = read_index(fx.path);
  const Rect r{300, 300, 100, 100};
  const auto ids = expected_blocks(cfg, 1024, 1024, 5, 0, r);
  std::vector<char> bytes;
  {
    std::ifstream f(fx.path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(f), {});
  }
  const uint32_t victim = *ids.begin();
  std::mt19937 garbage(9);
  for (uint64_t o = ii.offsets[victim]; o < ii.offsets[victim + 1]; ++o)
    bytes[size_t(ii.data_offset + o)] = char(garbage());
  const auto p = testutil::temp_path(std::string("victim_") + cfg.name + ".prmd").string();
  std::ofstream(p, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
  auto dec = open_decoder(p);
  bool differs = false;
  try {
    differs = dec->decode(0, r).image.pixels != fx.ref[0].crop(r).pixels;
  } catch (const std::exception&) {
    differs = true;
  }
  EXPECT_TRUE(differs) << cfg.name;
}

INSTANTIATE_TEST_SUITE_P(All, RandomAccessTest, ::testing::ValuesIn(testutil::prmd_configs()),
                         [](const auto& info) { return std::string(info.param.name); });

TEST(RandomAccess, CacheServesRepeatedCrops) {
  const Fixture& fx = fixture(testutil::prmd_configs()[0], 1024, 1024, ColorFormat::Gray8);
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
  const Fixture& fx = fixture(testutil::prmd_configs()[4], 1024, 1024, ColorFormat::Gray8);
  auto dec = open_decoder(fx.path);
  const auto res = dec->decode(0, {10, 10, 100, 100});
  EXPECT_EQ(res.stats.blocks_decoded, 1);
  EXPECT_EQ(res.stats.tiles_touched, 1);
}

// ------------------------------------------------------------------ format integrity

TEST(Format, RejectsCorruptHeadersAndIndex) {
  const Fixture& fx = fixture(testutil::prmd_configs()[0], 1024, 1024, ColorFormat::Gray8);
  std::vector<char> good;
  {
    std::ifstream f(fx.path, std::ios::binary);
    good.assign(std::istreambuf_iterator<char>(f), {});
  }
  auto try_open = [&](std::vector<char> bytes) {
    const auto p = testutil::temp_path("bad.prmd").string();
    std::ofstream(p, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    return open_decoder(p);
  };
  EXPECT_NO_THROW(try_open(good));
  auto bad = good;
  bad[0] = 'X';
  EXPECT_THROW(try_open(bad), FormatError);  // magic
  bad = good;
  bad[4] = 9;
  EXPECT_THROW(try_open(bad), FormatError);  // version
  bad = good;
  bad[17] = 7;
  EXPECT_THROW(try_open(bad), FormatError);  // color id
  bad = good;
  bad[40] = 9;
  EXPECT_THROW(try_open(bad), FormatError);  // entropy id
  bad = good;
  bad[internal::kHeaderSize + 3] ^= 1;
  EXPECT_THROW(try_open(bad), FormatError);  // index CRC
  bad = good;
  bad.resize(bad.size() - 1);
  EXPECT_THROW(try_open(bad), FormatError);  // truncated data
  bad.assign(good.begin(), good.begin() + 30);
  EXPECT_THROW(try_open(bad), FormatError);  // truncated header
}

TEST(Format, EncoderValidatesOptions) {
  const Image img(16, 16);
  EncodeOptions o;
  const auto p = testutil::temp_path("opts.prmd").string();
  o.levels = 0;
  EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
  o = {};
  o.cblk = 100;
  o.tile = 256;
  EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
  o = {};
  o.quant = -1;
  EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
  o = {};
  o.quant = 4;
  o.transform = Transform::Independent;  // naive is lossless only
  EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
  o = {};
  o.entropy = Entropy::Range;
  o.entropy_level = 5;  // range takes no level
  EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
  o = {};
  o.entropy_level = 23;  // zstd: 1..22
  EXPECT_THROW(encode_file(img, p, Codec::Prmd, o), std::invalid_argument);
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
  const auto haar = fixture(cfgs[0], 1024, 1024, ColorFormat::Gray8).report.file_bytes;
  const auto cdf = fixture(cfgs[1], 1024, 1024, ColorFormat::Gray8).report.file_bytes;
  const auto naive = fixture(cfgs[4], 1024, 1024, ColorFormat::Gray8).report.file_bytes;
  EXPECT_LT(haar, naive);
  EXPECT_LT(cdf, naive);
}
