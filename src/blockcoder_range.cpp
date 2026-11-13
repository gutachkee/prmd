// Context-adaptive binary range coder for code-blocks.
//
// Per signed value v:
//   zero flag            ctx: (model, bucket)
//   sign                 ctx: (model)
//   u = |v| (>= 1), k = floor(log2 u)
//   k in unary           ctx: (model, bucket, min(i, kPrefixCtx-1))
//   top 2 bits below MSB ctx: (model, k, position)
//   remaining k-2 bits   direct
// model = band kind × (luma | chroma); bucket from |left| + |up| + (|up-left| + |up-right|)/2
// in the same plane (out-of-block neighbours are 0).

#include <array>
#include <cstdlib>
#include <memory>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/rangecoder.h"

namespace prmd::internal {
namespace {

constexpr int kBuckets = 8;
constexpr int kPrefixCtx = 20;
constexpr int kMaxK = 31;

struct Model {
  std::array<Prob, kBuckets> zero;
  Prob sign;
  std::array<std::array<Prob, kPrefixCtx>, kBuckets> prefix;
  std::array<std::array<Prob, 3>, kMaxK + 1> suffix;  // [k][0]=first bit, [k][1|2]=second bit given first
  Model() {
    zero.fill(kProbInit);
    sign = kProbInit;
    for (auto& a : prefix) a.fill(kProbInit);
    for (auto& a : suffix) a.fill(kProbInit);
  }
};

inline int bucket_of(uint32_t s) {
  if (s == 0) return 0;
  if (s == 1) return 1;
  if (s == 2) return 2;
  if (s <= 4) return 3;
  if (s <= 8) return 4;
  if (s <= 16) return 5;
  if (s <= 40) return 6;
  return 7;
}

inline int floor_log2(uint32_t u) { return 31 - __builtin_clz(u); }

inline uint32_t mag(int32_t v) { return v < 0 ? uint32_t(-int64_t(v)) : uint32_t(v); }

inline int context(const int32_t* p, int w, int x, int y) {
  uint32_t s = 0;
  if (x > 0) s += mag(p[size_t(y) * size_t(w) + size_t(x - 1)]);
  if (y > 0) {
    const int32_t* up = p + size_t(y - 1) * size_t(w);
    s += mag(up[x]);
    uint32_t d = 0;
    if (x > 0) d += mag(up[x - 1]);
    if (x + 1 < w) d += mag(up[x + 1]);
    s += d >> 1;
  }
  return bucket_of(s);
}

inline size_t model_index(Band b, int channel) { return size_t(b) * 2 + (channel == 0 ? 0 : 1); }

class RangeBlockCoder final : public BlockCoder {
 public:
  Entropy id() const override { return Entropy::Range; }

  void encode(const BlockPlanes& in, std::vector<uint8_t>& out) const override {
    std::array<Model, kNumBandKinds * 2> models;  // fresh per block
    RangeEncoder enc(out);
    for (size_t pi = 0; pi < in.planes.size(); ++pi) {
      Model& m = models[model_index(in.kinds[pi], int(pi % size_t(in.channels)))];
      const int32_t* p = in.planes[pi].data();
      for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
          const int32_t v = p[size_t(y) * size_t(in.w) + size_t(x)];
          const int b = context(p, in.w, x, y);
          if (v == 0) {
            enc.encode_bit(m.zero[size_t(b)], 0);
            continue;
          }
          enc.encode_bit(m.zero[size_t(b)], 1);
          enc.encode_bit(m.sign, v < 0 ? 1 : 0);
          const uint32_t u = mag(v);
          const int k = floor_log2(u);
          for (int i = 0; i < k; ++i) enc.encode_bit(m.prefix[size_t(b)][size_t(std::min(i, kPrefixCtx - 1))], 1);
          enc.encode_bit(m.prefix[size_t(b)][size_t(std::min(k, kPrefixCtx - 1))], 0);
          int rem = k;
          if (rem >= 1) {
            const int b1 = int((u >> (rem - 1)) & 1u);
            enc.encode_bit(m.suffix[size_t(k)][0], b1);
            --rem;
            if (rem >= 1) {
              enc.encode_bit(m.suffix[size_t(k)][size_t(1 + b1)], int((u >> (rem - 1)) & 1u));
              --rem;
            }
          }
          while (rem > 0) {
            const int nb = rem > 24 ? 24 : rem;
            enc.encode_direct((u >> (rem - nb)) & ((1u << nb) - 1u), nb);
            rem -= nb;
          }
        }
    }
    enc.finish();
  }

  void decode(const uint8_t* data, size_t size, BlockPlanes& io) const override {
    std::array<Model, kNumBandKinds * 2> models;
    RangeDecoder dec(data, size);
    const size_t n = size_t(io.w) * size_t(io.h);
    io.planes.assign(io.kinds.size(), std::vector<int32_t>(n));
    for (size_t pi = 0; pi < io.planes.size(); ++pi) {
      Model& m = models[model_index(io.kinds[pi], int(pi % size_t(io.channels)))];
      int32_t* p = io.planes[pi].data();
      for (int y = 0; y < io.h; ++y)
        for (int x = 0; x < io.w; ++x) {
          const int b = context(p, io.w, x, y);
          int32_t v = 0;
          if (dec.decode_bit(m.zero[size_t(b)])) {
            const bool neg = dec.decode_bit(m.sign) != 0;
            int k = 0;
            while (dec.decode_bit(m.prefix[size_t(b)][size_t(std::min(k, kPrefixCtx - 1))])) {
              if (++k > kMaxK - 1) throw FormatError("range block: magnitude prefix too long");
            }
            uint32_t u = 1;
            int rem = k;
            if (rem >= 1) {
              const int b1 = dec.decode_bit(m.suffix[size_t(k)][0]);
              u = (u << 1) | uint32_t(b1);
              --rem;
              if (rem >= 1) {
                u = (u << 1) | uint32_t(dec.decode_bit(m.suffix[size_t(k)][size_t(1 + b1)]));
                --rem;
              }
            }
            while (rem > 0) {
              const int nb = rem > 24 ? 24 : rem;
              u = (u << nb) | dec.decode_direct(nb);
              rem -= nb;
            }
            v = neg ? int32_t(-int64_t(u)) : int32_t(u);
          }
          p[size_t(y) * size_t(io.w) + size_t(x)] = v;
        }
    }
    if (dec.overrun() > 0) throw FormatError("range block: truncated payload");
  }
};

}  // namespace

std::unique_ptr<BlockCoder> make_range_coder() { return std::make_unique<RangeBlockCoder>(); }

}  // namespace prmd::internal
