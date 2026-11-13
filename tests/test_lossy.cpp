// Lossy .prmd (quant > 0).
//
// What is checked:
//  - Random-access consistency: every crop equals the same crop of the
//    decoder's own full-level decode (seamless across tiles/blocks, identical
//    regardless of the requested rect).
//  - Pipeline correctness: the decoded levels equal an independent simulation
//    of the documented lossy scheme (quantize bands → dequantize → inverse
//    transforms), built in this test from the exact pyramid without any of the
//    codec's block/format/entropy code.
//  - Laplacian closed loop: on gray, every level stays within ±q/2 of the
//    exact pyramid.
//  - Quality/size sanity: PSNR thresholds and monotone file sizes in q.

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <random>
#include <tuple>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/planes.h"
#include "prmd/internal/transform.h"
#include "reference.h"
#include "test_util.h"

using namespace prmd;
using namespace prmd::internal;
using testutil::Config;

namespace {

const std::vector<Config>& lossy_configs() {
  static const std::vector<Config> c = {
      {"haar", Transform::Haar, Downsampler::Box, Entropy::Zstd},
      {"cdf53", Transform::Cdf53, Downsampler::Box, Entropy::Zstd},
      {"laplacian_box", Transform::Laplacian, Downsampler::Box, Entropy::Zstd},
      {"laplacian_gauss", Transform::Laplacian, Downsampler::Gauss,
       Entropy::Zstd},
      {"haar_range", Transform::Haar, Downsampler::Box, Entropy::Range},
  };
  return c;
}

double psnr(const Image& a, const Image& b) {
  double se = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    const double d = double(a.pixels[i]) - double(b.pixels[i]);
    se += d * d;
  }
  if (se == 0) {
    return 99.0;
  }
  return 10.0 * std::log10(255.0 * 255.0 * double(a.pixels.size()) / se);
}

int max_abs_error(const Image& a, const Image& b) {
  int m = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    m = std::max(m, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
  }
  return m;
}

struct LossyFixture {
  Image src;
  std::vector<Image> exact;    // lossless reference pyramid
  std::vector<Image> decoded;  // full-level decodes of the lossy file
  std::string path;
  uint64_t bytes = 0;
};

const LossyFixture& lossy_fixture(const Config& cfg, ColorFormat f, int q) {
  static std::map<std::string, LossyFixture> cache;
  const std::string key =
      std::string(cfg.name) + "_" + info(f).name + "_q" + std::to_string(q);
  auto it = cache.find(key);
  if (it != cache.end()) {
    return it->second;
  }

  const bool gray = f == ColorFormat::Gray8;
  const int w = gray ? 700 : 400;
  const int h = gray ? 520 : 300;

  LossyFixture fx;
  fx.src = ref::synthetic_image(w, h, f, 99);
  fx.exact = ref::pyramid(fx.src, 5, cfg.t, cfg.d);
  fx.path = testutil::temp_path("lossy_" + key + ".prmd").string();

  EncodeOptions o = testutil::options_for(cfg, 5, f);
  o.quant = q;
  fx.bytes = encode_file(fx.src, fx.path, Codec::Prmd, o).file_bytes;

  auto dec = open_decoder(fx.path);
  for (int L = 0; L < 5; ++L) {
    const LevelInfo li = dec->level_info(L);
    fx.decoded.push_back(dec->decode(L, {0, 0, li.width, li.height}).image);
  }
  return cache.emplace(key, std::move(fx)).first->second;
}

// What the decoder sees of one band (one plane per channel):
// color → quantize → dequantize → inverse color.
void lossy_roundtrip(std::vector<Region>& channels, int step) {
  const bool rgb = channels.size() == 3;
  if (rgb) {
    ycocg_r_forward(channels[0].v.data(), channels[1].v.data(),
                    channels[2].v.data(), channels[0].v.size());
  }
  for (auto& r : channels) {
    for (auto& v : r.v) {
      v = dequantize(quantize(v, step), step);
    }
  }
  if (rgb) {
    ycocg_r_inverse(channels[0].v.data(), channels[1].v.data(),
                    channels[2].v.data(), channels[0].v.size());
  }
}

// Independent simulation of the documented lossy scheme on whole planes (no
// blocks, no entropy coding, no file format). Returns the expected decoded
// level images.
std::vector<Image> simulate(const Image& src,
                            const Config& cfg,
                            int q,
                            int levels) {
  const auto xf = make_transform(cfg.t, cfg.d);
  const int C = src.channels();
  const int nb = xf->num_bands();
  const auto exact = build_level_planes(src, levels, cfg.t, cfg.d);

  std::vector<std::vector<Region>> recon(static_cast<size_t>(levels));
  recon[size_t(levels - 1)] = exact[size_t(levels - 1)];

  for (int L = levels - 2; L >= 0; --L) {
    const Region& f0 = exact[size_t(L)][0];
    const Dims fd{f0.rect.w, f0.rect.h};
    const std::vector<Region>& coarse = recon[size_t(L + 1)];
    // [band][channel]
    std::vector<std::vector<Region>> bands(static_cast<size_t>(nb));

    if (cfg.t == Transform::Laplacian) {
      // Closed loop: residual vs U(reconstructed coarse).
      for (int c = 0; c < C; ++c) {
        Region pred;
        xf->inverse(coarse[size_t(c)], {Region(f0.rect)}, f0.rect, fd, pred);
        Region r(f0.rect);
        for (size_t i = 0; i < r.v.size(); ++i) {
          r.v[i] = exact[size_t(L)][size_t(c)].v[i] - pred.v[i];
        }
        bands[0].push_back(r);
      }
    } else {
      // Open loop: bands of the exact level.
      for (int c = 0; c < C; ++c) {
        Region unused_coarse;
        std::vector<Region> b;
        xf->forward(exact[size_t(L)][size_t(c)], unused_coarse, b);
        for (int k = 0; k < nb; ++k) {
          bands[size_t(k)].push_back(b[size_t(k)]);
        }
      }
    }

    for (int k = 0; k < nb; ++k) {
      lossy_roundtrip(bands[size_t(k)], band_step(xf->band_kind(k), q));
    }

    for (int c = 0; c < C; ++c) {
      std::vector<Region> channel_bands;
      for (int k = 0; k < nb; ++k) {
        channel_bands.push_back(bands[size_t(k)][size_t(c)]);
      }
      Region out;
      xf->inverse(coarse[size_t(c)], channel_bands, f0.rect, fd, out);
      recon[size_t(L)].push_back(std::move(out));
    }
  }

  std::vector<Image> out;
  for (const auto& lv : recon) {
    out.push_back(from_planes(lv, src.format));
  }
  return out;
}

}  // namespace

class LossyTest
    : public ::testing::TestWithParam<std::tuple<Config, ColorFormat, int>> {};

TEST_P(LossyTest, MatchesIndependentSimulation) {
  const auto [cfg, fmt, q] = GetParam();
  const LossyFixture& fx = lossy_fixture(cfg, fmt, q);
  const auto expected = simulate(fx.src, cfg, q, 5);
  for (int L = 0; L < 5; ++L) {
    ASSERT_EQ(fx.decoded[size_t(L)].pixels, expected[size_t(L)].pixels)
        << "L" << L;
  }
}

TEST_P(LossyTest, CropsAreSeamlessAndConsistent) {
  const auto [cfg, fmt, q] = GetParam();
  const LossyFixture& fx = lossy_fixture(cfg, fmt, q);
  auto dec = open_decoder(fx.path);
  std::mt19937 rng(17);

  for (int i = 0; i < 150; ++i) {
    const int L = int(rng() % 5u);
    const LevelInfo li = dec->level_info(L);
    const Rect r = testutil::random_rect(rng, li.width, li.height, 300);
    ASSERT_EQ(dec->decode(L, r).image.pixels,
              fx.decoded[size_t(L)].crop(r).pixels)
        << "L" << L << " " << testutil::to_string(r);
  }
}

TEST_P(LossyTest, QualityAndSize) {
  const auto [cfg, fmt, q] = GetParam();
  const LossyFixture& fx = lossy_fixture(cfg, fmt, q);
  const LossyFixture& fine = lossy_fixture(cfg, fmt, 2);

  // Coarsest level is stored losslessly.
  EXPECT_EQ(fx.decoded[4].pixels, fx.exact[4].pixels);

  const double p0 = psnr(fx.decoded[0], fx.exact[0]);
  EXPECT_GT(p0, q == 2 ? 40.0 : 30.0) << "L0 PSNR " << p0;
  if (q > 2) {
    EXPECT_LT(fx.bytes, fine.bytes);
  }

  // Lossless is larger than any lossy setting.
  const auto lossless = testutil::temp_path(std::string("lossless_") +
                                            cfg.name + info(fmt).name + ".prmd")
                            .string();
  const EncodeOptions lossless_options = testutil::options_for(cfg, 5, fmt);
  const uint64_t lossless_bytes =
      encode_file(fx.src, lossless, Codec::Prmd, lossless_options).file_bytes;
  EXPECT_LT(fx.bytes, lossless_bytes);
}

INSTANTIATE_TEST_SUITE_P(
    All,
    LossyTest,
    ::testing::Combine(::testing::ValuesIn(lossy_configs()),
                       ::testing::Values(ColorFormat::Gray8, ColorFormat::RGB8),
                       ::testing::Values(2, 8)),
    [](const auto& info) {
      return std::string(std::get<0>(info.param).name) + "_" +
             prmd::info(std::get<1>(info.param)).name + "_q" +
             std::to_string(std::get<2>(info.param));
    });

// Closed-loop guarantee: on gray, a Laplacian level never deviates by more
// than q/2.
TEST(Lossy, LaplacianErrorIsBoundedPerLevel) {
  for (const auto& cfg : lossy_configs()) {
    if (cfg.t != Transform::Laplacian) {
      continue;
    }
    for (int q : {2, 5, 8, 16}) {
      const LossyFixture& fx = lossy_fixture(cfg, ColorFormat::Gray8, q);
      for (int L = 0; L < 5; ++L) {
        const int max_err =
            max_abs_error(fx.decoded[size_t(L)], fx.exact[size_t(L)]);
        EXPECT_LE(max_err, q / 2) << cfg.name << " q" << q << " L" << L;
      }
    }
  }
}

TEST(Lossy, QuantizerRoundTripBound) {
  for (int step : {2, 3, 4, 7, 16}) {
    for (int v = -600; v <= 600; ++v) {
      const int r = dequantize(quantize(v, step), step);
      ASSERT_LE(std::abs(r - v), step / 2) << "v=" << v << " step=" << step;
    }
  }
  EXPECT_EQ(quantize(-37, 0), -37);
  EXPECT_EQ(dequantize(5, 1), 5);
}
