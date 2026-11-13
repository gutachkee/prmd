#include <gtest/gtest.h>

#include <random>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"

using namespace prmd;
using namespace prmd::internal;

namespace {

BlockPlanes make_planes(int w, int h, int channels, const std::vector<Band>& band_kinds, int lo, int hi,
                        unsigned seed, bool laplacian_dist) {
  BlockPlanes bp;
  bp.w = w;
  bp.h = h;
  bp.channels = channels;
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> U(lo, hi);
  std::geometric_distribution<int> G(0.4);
  for (Band b : band_kinds)
    for (int c = 0; c < channels; ++c) {
      bp.kinds.push_back(b);
      std::vector<int32_t> p(size_t(w) * size_t(h));
      for (auto& v : p) {
        if (laplacian_dist) {
          const int m = G(rng);
          v = (rng() & 1) ? -m : m;
        } else {
          v = U(rng);
        }
      }
      bp.planes.push_back(std::move(p));
    }
  return bp;
}

void expect_roundtrip(const BlockCoder& coder, const BlockPlanes& in) {
  std::vector<uint8_t> bytes;
  coder.encode(in, bytes);
  BlockPlanes out;
  out.w = in.w;
  out.h = in.h;
  out.channels = in.channels;
  out.kinds = in.kinds;
  coder.decode(bytes.data(), bytes.size(), out);
  ASSERT_EQ(out.planes, in.planes);
}

}  // namespace

class BlockCoderTest : public ::testing::TestWithParam<Entropy> {};

TEST_P(BlockCoderTest, RoundTripsAllBandKindsAndShapes) {
  const auto coder = make_block_coder(GetParam());
  const std::vector<Band> detail = {Band::LH, Band::HL, Band::HH};
  for (int ch : {1, 3}) {
    for (auto [w, h] : {std::pair{64, 64}, std::pair{1, 1}, std::pair{13, 7}, std::pair{128, 3}}) {
      expect_roundtrip(*coder, make_planes(w, h, ch, detail, -510, 510, unsigned(w + h + ch), false));
      expect_roundtrip(*coder, make_planes(w, h, ch, detail, 0, 0, 1, true));
      expect_roundtrip(*coder, make_planes(w, h, ch, {Band::Base}, -255, 255, 2, true));
      expect_roundtrip(*coder, make_planes(w, h, ch, {Band::Residual}, -2040, 2040, 3, false));
    }
  }
}

TEST_P(BlockCoderTest, ExtremeValues) {
  const auto coder = make_block_coder(GetParam());
  BlockPlanes bp = make_planes(4, 2, 1, {Band::HH}, 0, 0, 1, false);
  bp.planes[0] = {-32768, 32767, 0, 1, -1, 2040, -2040, 510};
  expect_roundtrip(*coder, bp);
}

TEST_P(BlockCoderTest, TruncatedPayloadThrows) {
  const auto coder = make_block_coder(GetParam());
  const BlockPlanes in = make_planes(32, 32, 1, {Band::LH}, -300, 300, 9, false);
  std::vector<uint8_t> bytes;
  coder->encode(in, bytes);
  bytes.resize(bytes.size() / 3);
  BlockPlanes out;
  out.w = 32;
  out.h = 32;
  out.kinds = in.kinds;
  EXPECT_THROW(coder->decode(bytes.data(), bytes.size(), out), FormatError);
}

INSTANTIATE_TEST_SUITE_P(All, BlockCoderTest, ::testing::Values(Entropy::Range, Entropy::Zstd, Entropy::Raw),
                         [](const auto& info) { return std::string(to_string(info.param)); });

TEST(BlockCoder, RangeHandlesFullInt32Magnitudes) {
  const auto coder = make_block_coder(Entropy::Range);
  BlockPlanes bp = make_planes(3, 1, 1, {Band::HH}, 0, 0, 1, false);
  bp.planes[0] = {2147483647, -2147483647, 1 << 24};
  expect_roundtrip(*coder, bp);
}

TEST(BlockCoder, ByteCodersRejectOutOfInt16) {
  for (Entropy e : {Entropy::Zstd, Entropy::Raw}) {
    const auto coder = make_block_coder(e);
    BlockPlanes bp = make_planes(1, 1, 1, {Band::HH}, 0, 0, 1, false);
    bp.planes[0] = {40000};
    std::vector<uint8_t> out;
    EXPECT_THROW(coder->encode(bp, out), std::invalid_argument);
  }
}

TEST(BlockCoder, FactoryValidatesLevels) {
  EXPECT_THROW(make_block_coder(Entropy::Range, 3), std::invalid_argument);
  EXPECT_THROW(make_block_coder(Entropy::Zstd, 23), std::invalid_argument);
  EXPECT_NO_THROW(make_block_coder(Entropy::Zstd, 3));
  EXPECT_THROW(make_block_coder(Entropy(9)), std::invalid_argument);
}

TEST(BlockCoder, RangeBeatsRawOnSkewedData) {
  const BlockPlanes in = make_planes(128, 128, 1, {Band::LH, Band::HL, Band::HH}, 0, 0, 5, true);
  std::vector<uint8_t> r, z;
  make_block_coder(Entropy::Range)->encode(in, r);
  make_block_coder(Entropy::Raw)->encode(in, z);
  EXPECT_LT(r.size() * 4, z.size());
}

TEST(ColorTransform, YCoCgRRoundTrip) {
  std::mt19937 rng(1);
  std::uniform_int_distribution<int> U(-1020, 1020);
  std::vector<int32_t> a(5000), b(5000), c(5000);
  for (size_t i = 0; i < a.size(); ++i) a[i] = U(rng), b[i] = U(rng), c[i] = U(rng);
  a[0] = 255, b[0] = 0, c[0] = 255;
  auto a0 = a, b0 = b, c0 = c;
  ycocg_r_forward(a.data(), b.data(), c.data(), a.size());
  ycocg_r_inverse(a.data(), b.data(), c.data(), a.size());
  EXPECT_EQ(a, a0);
  EXPECT_EQ(b, b0);
  EXPECT_EQ(c, c0);
}

TEST(Med, RoundTrip) {
  std::mt19937 rng(2);
  for (auto [w, h] : {std::pair{1, 1}, std::pair{9, 4}, std::pair{128, 128}}) {
    std::vector<int32_t> px(size_t(w) * size_t(h)), res(px.size()), back(px.size());
    for (auto& v : px) v = int32_t(rng() % 600) - 100;
    med_residual(px.data(), w, h, res.data());
    med_reconstruct(res.data(), w, h, back.data());
    EXPECT_EQ(back, px);
  }
}
