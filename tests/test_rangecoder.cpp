#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "prmd/internal/rangecoder.h"

using namespace prmd::internal;

namespace {

struct Sym {
  bool direct;
  int ctx;         // context index for adaptive bits
  uint32_t value;  // bit (adaptive) or value (direct)
  int nbits;       // direct only
};

std::vector<uint8_t> encode(const std::vector<Sym>& syms, int nctx) {
  std::vector<uint8_t> out;
  RangeEncoder enc(out);
  std::vector<Prob> probs(size_t(nctx), kProbInit);
  for (const auto& s : syms) {
    if (s.direct) {
      enc.encode_direct(s.value, s.nbits);
    } else {
      enc.encode_bit(probs[size_t(s.ctx)], int(s.value));
    }
  }
  enc.finish();
  return out;
}

void expect_roundtrip(const std::vector<Sym>& syms, int nctx) {
  const auto bytes = encode(syms, nctx);
  RangeDecoder dec(bytes.data(), bytes.size());
  std::vector<Prob> probs(size_t(nctx), kProbInit);
  for (size_t i = 0; i < syms.size(); ++i) {
    const auto& s = syms[i];
    const uint32_t got = s.direct
                             ? dec.decode_direct(s.nbits)
                             : uint32_t(dec.decode_bit(probs[size_t(s.ctx)]));
    ASSERT_EQ(got, s.value) << "symbol " << i;
  }
  EXPECT_EQ(dec.overrun(), 0u);
}

}  // namespace

TEST(RangeCoder, EmptyStream) {
  expect_roundtrip({}, 1);
}

TEST(RangeCoder, RandomBitsManyContexts) {
  std::mt19937 rng(1);
  std::vector<Sym> syms;
  for (int i = 0; i < 200000; ++i) {
    syms.push_back({false, int(rng() % 64), uint32_t(rng() & 1u), 0});
  }
  expect_roundtrip(syms, 64);
}

TEST(RangeCoder, SkewedBitsCompress) {
  std::mt19937 rng(2);
  std::bernoulli_distribution one(0.02);
  std::vector<Sym> syms;
  const int n = 100000;
  for (int i = 0; i < n; ++i) {
    syms.push_back({false, 0, uint32_t(one(rng)), 0});
  }
  expect_roundtrip(syms, 1);
  // Entropy of p=0.02 is ~0.14 bit/symbol; allow adaptation overhead.
  const auto bytes = encode(syms, 1);
  EXPECT_LT(bytes.size() * 8.0 / n, 0.2);
}

TEST(RangeCoder, AllZerosIsTiny) {
  std::vector<Sym> syms(100000, Sym{false, 0, 0, 0});
  expect_roundtrip(syms, 1);
  EXPECT_LT(encode(syms, 1).size(), 300u);
}

TEST(RangeCoder, DirectBitsAllWidths) {
  std::mt19937 rng(3);
  std::vector<Sym> syms;
  for (int i = 0; i < 50000; ++i) {
    const int nb = int(rng() % 25);  // 0..24
    const uint32_t v = nb == 0 ? 0u : (rng() & ((1u << nb) - 1u));
    syms.push_back({true, 0, v, nb});
  }
  expect_roundtrip(syms, 1);
}

TEST(RangeCoder, MixedAdaptiveAndDirect) {
  std::mt19937 rng(4);
  std::vector<Sym> syms;
  for (int i = 0; i < 100000; ++i) {
    if (rng() % 3 == 0) {
      const int nb = 1 + int(rng() % 16);
      syms.push_back({true, 0, uint32_t(rng() & ((1u << nb) - 1u)), nb});
    } else {
      syms.push_back({false, int(rng() % 8), uint32_t(rng() % 10 == 0), 0});
    }
  }
  expect_roundtrip(syms, 8);
}

TEST(RangeCoder, CarryPropagationStress) {
  // Long runs of highly probable 1s push `low` toward carries through 0xFF
  // bytes.
  std::vector<Sym> syms;
  for (int r = 0; r < 50; ++r) {
    for (int i = 0; i < 3000; ++i) {
      syms.push_back({false, 0, 1, 0});
    }
    syms.push_back({false, 0, 0, 0});
    syms.push_back({true, 0, 0xFFFFFFu, 24});
  }
  expect_roundtrip(syms, 1);
}

TEST(RangeCoder, TruncatedInputDoesNotCrash) {
  std::mt19937 rng(5);
  std::vector<Sym> syms;
  for (int i = 0; i < 10000; ++i) {
    syms.push_back({false, 0, uint32_t(rng() & 1u), 0});
  }
  auto bytes = encode(syms, 1);
  bytes.resize(bytes.size() / 2);
  RangeDecoder dec(bytes.data(), bytes.size());
  Prob p = kProbInit;
  for (int i = 0; i < 10000; ++i) {
    (void)dec.decode_bit(p);
  }
  EXPECT_GT(dec.overrun(), 0u);
}
