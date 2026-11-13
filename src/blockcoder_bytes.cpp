// Byte-oriented block coders: Zstd and Raw. Both store values as zigzag-mapped uint16.
// Layout of the uncompressed stream: for each plane, the low bytes of all samples, then
// the high bytes (the high-byte planes are almost all zero, which zstd exploits).

#include <memory>
#include <stdexcept>
#include <string>

#include <zstd.h>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"

namespace prmd::internal {
namespace {

void pack(const BlockPlanes& in, std::vector<uint8_t>& buf) {
  const size_t n = size_t(in.w) * size_t(in.h);
  buf.resize(in.planes.size() * n * 2);
  uint8_t* o = buf.data();
  for (const auto& p : in.planes) {
    for (size_t i = 0; i < n; ++i) {
      const int32_t v = p[i];
      const uint32_t z = (uint32_t(v) << 1) ^ uint32_t(v >> 31);
      if (z > 0xFFFFu) throw std::invalid_argument("zstd/raw block coder: value " + std::to_string(v) +
                                                   " does not fit in int16");
      o[i] = uint8_t(z);
      o[n + i] = uint8_t(z >> 8);
    }
    o += 2 * n;
  }
}

void unpack(const uint8_t* buf, BlockPlanes& io) {
  const size_t n = size_t(io.w) * size_t(io.h);
  io.planes.assign(io.kinds.size(), std::vector<int32_t>(n));
  for (auto& p : io.planes) {
    for (size_t i = 0; i < n; ++i) {
      const uint32_t z = uint32_t(buf[i]) | (uint32_t(buf[n + i]) << 8);
      p[i] = int32_t(z >> 1) ^ -int32_t(z & 1u);
    }
    buf += 2 * n;
  }
}

size_t packed_size(const BlockPlanes& io) { return io.kinds.size() * size_t(io.w) * size_t(io.h) * 2; }

class RawBlockCoder final : public BlockCoder {
 public:
  Entropy id() const override { return Entropy::Raw; }
  void encode(const BlockPlanes& in, std::vector<uint8_t>& out) const override {
    std::vector<uint8_t> buf;
    pack(in, buf);
    out.insert(out.end(), buf.begin(), buf.end());
  }
  void decode(const uint8_t* data, size_t size, BlockPlanes& io) const override {
    if (size != packed_size(io)) throw FormatError("raw block: size mismatch");
    unpack(data, io);
  }
};

class ZstdBlockCoder final : public BlockCoder {
 public:
  explicit ZstdBlockCoder(int level) : level_(level) {}
  Entropy id() const override { return Entropy::Zstd; }
  void encode(const BlockPlanes& in, std::vector<uint8_t>& out) const override {
    std::vector<uint8_t> buf;
    pack(in, buf);
    const size_t start = out.size();
    out.resize(start + ZSTD_compressBound(buf.size()));
    const size_t r = ZSTD_compress(out.data() + start, out.size() - start, buf.data(), buf.size(), level_);
    if (ZSTD_isError(r)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(r));
    out.resize(start + r);
  }
  void decode(const uint8_t* data, size_t size, BlockPlanes& io) const override {
    std::vector<uint8_t> buf(packed_size(io));
    const size_t r = ZSTD_decompress(buf.data(), buf.size(), data, size);
    if (ZSTD_isError(r) || r != buf.size()) throw FormatError("zstd block: corrupt payload");
    unpack(buf.data(), io);
  }

 private:
  int level_;
};

}  // namespace

std::unique_ptr<BlockCoder> make_zstd_coder(int level) { return std::make_unique<ZstdBlockCoder>(level); }
std::unique_ptr<BlockCoder> make_raw_coder() { return std::make_unique<RawBlockCoder>(); }

}  // namespace prmd::internal
