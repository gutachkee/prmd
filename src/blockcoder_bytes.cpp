// Byte-oriented block coders: Zstd and Raw. Both store values as
// zigzag-mapped uint16. Layout of the uncompressed stream: for each plane, the
// low bytes of all samples, then the high bytes (the high-byte planes are
// almost all zero, which zstd exploits).

#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include <zstd.h>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/profile.h"

namespace prmd::internal {
namespace {

// One zstd context per thread, reused across blocks: ZSTD_compress and
// ZSTD_decompress would otherwise allocate and free a context per call.
struct CCtxFree {
  void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};
struct DCtxFree {
  void operator()(ZSTD_DCtx* c) const { ZSTD_freeDCtx(c); }
};

ZSTD_CCtx* thread_cctx() {
  thread_local std::unique_ptr<ZSTD_CCtx, CCtxFree> ctx(ZSTD_createCCtx());
  if (!ctx) {
    throw std::bad_alloc();
  }
  return ctx.get();
}

ZSTD_DCtx* thread_dctx() {
  thread_local std::unique_ptr<ZSTD_DCtx, DCtxFree> ctx(ZSTD_createDCtx());
  if (!ctx) {
    throw std::bad_alloc();
  }
  return ctx.get();
}

// Per-thread buffer for the packed stream of encode(), reused across blocks
// (one allocation per thread instead of one per block). Released after use
// when a huge code-block grew it past kMaxKeptPack.
constexpr size_t kMaxKeptPack = size_t(8) << 20;

std::vector<uint8_t>& pack_scratch() {
  thread_local std::vector<uint8_t> buf;
  return buf;
}

void trim_pack_scratch() {
  std::vector<uint8_t>& buf = pack_scratch();
  if (buf.capacity() > kMaxKeptPack) {
    std::vector<uint8_t>().swap(buf);
  }
}

inline uint32_t zigzag_encode(int32_t v) {
  return (uint32_t(v) << 1) ^ uint32_t(v >> 31);
}

inline int32_t zigzag_decode(uint32_t z) {
  return int32_t(z >> 1) ^ -int32_t(z & 1u);
}

size_t samples_per_plane(const BlockPlanes& bp) {
  return size_t(bp.w) * size_t(bp.h);
}

// Size of the packed stream for the planes described by `io`.
size_t packed_size(const BlockPlanes& io) {
  return io.kinds.size() * samples_per_plane(io) * 2;
}

void pack(const BlockPlanes& in, std::vector<uint8_t>& buf) {
  PRMD_TIMED("pack");
  const size_t n = samples_per_plane(in);
  buf.resize(in.planes.size() * n * 2);

  uint8_t* o = buf.data();
  for (const auto& plane : in.planes) {
    for (size_t i = 0; i < n; ++i) {
      const int32_t v = plane[i];
      const uint32_t z = zigzag_encode(v);
      if (z > 0xFFFFu) {
        throw std::invalid_argument("zstd/raw block coder: value " +
                                    std::to_string(v) +
                                    " does not fit in int16");
      }
      o[i] = uint8_t(z);
      o[n + i] = uint8_t(z >> 8);
    }
    o += 2 * n;
  }
}

void unpack(const uint8_t* buf, BlockPlanes& io) {
  PRMD_TIMED("unpack");
  const size_t n = samples_per_plane(io);
  io.planes.assign(io.kinds.size(), std::vector<int32_t>(n));

  for (auto& plane : io.planes) {
    for (size_t i = 0; i < n; ++i) {
      plane[i] = zigzag_decode(uint32_t(buf[i]) | (uint32_t(buf[n + i]) << 8));
    }
    buf += 2 * n;
  }
}

class RawBlockCoder final : public BlockCoder {
 public:
  Entropy id() const override { return Entropy::Raw; }

  void encode(const BlockPlanes& in, std::vector<uint8_t>& out) const override {
    std::vector<uint8_t>& buf = pack_scratch();
    pack(in, buf);
    out.insert(out.end(), buf.begin(), buf.end());
    trim_pack_scratch();
  }

  void decode(const uint8_t* data,
              size_t size,
              BlockPlanes& io) const override {
    if (size != packed_size(io)) {
      throw FormatError("raw block: size mismatch");
    }
    unpack(data, io);
  }
};

class ZstdBlockCoder final : public BlockCoder {
 public:
  explicit ZstdBlockCoder(int level) : level_(level) {}

  Entropy id() const override { return Entropy::Zstd; }

  // Appends the frame to `out`, which is first grown by
  // ZSTD_compressBound and then shrunk to the frame (its capacity stays at
  // the bound: the encoder passes a reused per-thread buffer and keeps an
  // exact-size copy).
  void encode(const BlockPlanes& in, std::vector<uint8_t>& out) const override {
    std::vector<uint8_t>& buf = pack_scratch();
    pack(in, buf);

    PRMD_TIMED("zstd_compress");
    const size_t start = out.size();
    out.resize(start + ZSTD_compressBound(buf.size()));
    const size_t r =
        ZSTD_compressCCtx(thread_cctx(), out.data() + start,
                          out.size() - start, buf.data(), buf.size(), level_);
    if (ZSTD_isError(r)) {
      throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(r));
    }
    out.resize(start + r);
    trim_pack_scratch();
  }

  void decode(const uint8_t* data,
              size_t size,
              BlockPlanes& io) const override {
    std::vector<uint8_t> buf(packed_size(io));
    {
      PRMD_TIMED("zstd_decompress");
      const size_t r =
          ZSTD_decompressDCtx(thread_dctx(), buf.data(), buf.size(), data, size);
      if (ZSTD_isError(r) || r != buf.size()) {
        throw FormatError("zstd block: corrupt payload");
      }
    }
    unpack(buf.data(), io);
  }

 private:
  int level_;
};

}  // namespace

std::unique_ptr<BlockCoder> make_zstd_coder(int level) {
  return std::make_unique<ZstdBlockCoder>(level);
}

std::unique_ptr<BlockCoder> make_raw_coder() {
  return std::make_unique<RawBlockCoder>();
}

}  // namespace prmd::internal
