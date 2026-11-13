#include "prmd/internal/prmd_format.h"

#include <array>
#include <cstring>
#include <string>

#include "prmd/errors.h"

namespace prmd::internal {

namespace {

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32); }

std::array<uint32_t, 256> make_crc_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
  static const auto table = make_crc_table();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

std::vector<LevelGeom> make_geometry(int width, int height, int levels, int tile, int cblk,
                                     Transform t) {
  std::vector<LevelGeom> g(static_cast<size_t>(levels));
  int w = width, h = height;
  for (int L = 0; L < levels; ++L) {
    auto& lg = g[size_t(L)];
    lg.w = w;
    lg.h = h;
    lg.k = tile / cblk;
    lg.blocks_x = (w + cblk - 1) / cblk;
    lg.blocks_y = (h + cblk - 1) / cblk;
    lg.tiles_x = (w + tile - 1) / tile;
    lg.tiles_y = (h + tile - 1) / tile;
    lg.num_ids = uint32_t(lg.tiles_x * lg.tiles_y * lg.k * lg.k);
    lg.base = (L == levels - 1) || t == Transform::Independent;
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }
  // Data order: coarsest level first.
  uint32_t next = 0;
  for (int L = levels - 1; L >= 0; --L) {
    g[size_t(L)].first_block = next;
    next += g[size_t(L)].num_ids;
  }
  return g;
}

Rect block_pixel_rect(const LevelGeom& g, int cblk, int bx, int by) {
  return intersect(Rect{bx * cblk, by * cblk, cblk, cblk}, Rect{0, 0, g.w, g.h});
}

std::vector<uint8_t> serialize(PrmdHeader& h, const std::vector<LevelEntry>& levels,
                               const std::vector<uint32_t>& offsets) {
  const size_t idx = levels.size() * kLevelEntrySize + offsets.size() * 4;
  std::vector<uint8_t> b(kHeaderSize + idx, 0);
  size_t at = kHeaderSize;
  for (const auto& l : levels) {
    put32(b, at, l.width);
    put32(b, at + 4, l.height);
    put16(b, at + 8, l.blocks_x);
    put16(b, at + 10, l.blocks_y);
    put32(b, at + 12, l.first_block);
    at += kLevelEntrySize;
  }
  for (uint32_t o : offsets) {
    put32(b, at, o);
    at += 4;
  }
  h.index_crc32 = crc32(b.data() + kHeaderSize, idx);
  h.data_offset = kHeaderSize + idx;
  std::memcpy(b.data(), kMagic, 4);
  put16(b, 4, h.version);
  b[6] = uint8_t(h.transform);
  b[7] = uint8_t(h.downsampler);
  put32(b, 8, h.width);
  put32(b, 12, h.height);
  b[16] = h.levels;
  b[17] = uint8_t(h.color);
  put16(b, 18, h.tile);
  put16(b, 20, h.cblk);
  put16(b, 22, h.quant);
  put32(b, 24, h.total_blocks);
  put32(b, 28, h.index_crc32);
  put64(b, 32, h.data_offset);
  b[40] = uint8_t(h.entropy);
  b[41] = h.entropy_level;
  return b;
}

PrmdHeader parse_header(const uint8_t* p, size_t n) {
  if (n < kHeaderSize || std::memcmp(p, kMagic, 4) != 0) throw FormatError("not a .prmd file (bad magic)");
  PrmdHeader h;
  h.version = get16(p + 4);
  if (h.version != kVersion) throw FormatError("unsupported .prmd version " + std::to_string(h.version));
  if (p[6] > uint8_t(Transform::Independent)) throw FormatError("unknown transform id");
  h.transform = Transform(p[6]);
  if (p[7] > uint8_t(Downsampler::Gauss)) throw FormatError("unknown downsampler id");
  h.downsampler = Downsampler(p[7]);
  h.width = get32(p + 8);
  h.height = get32(p + 12);
  h.levels = p[16];
  if (!is_valid_color_format(p[17])) throw FormatError("unknown color format id");
  h.color = ColorFormat(p[17]);
  h.tile = get16(p + 18);
  h.cblk = get16(p + 20);
  h.quant = get16(p + 22);
  h.total_blocks = get32(p + 24);
  h.index_crc32 = get32(p + 28);
  h.data_offset = get64(p + 32);
  if (p[40] > uint8_t(Entropy::Raw)) throw FormatError("unknown entropy coder id");
  h.entropy = Entropy(p[40]);
  h.entropy_level = p[41];
  if (h.width == 0 || h.height == 0 || h.width > (1u << 30) || h.height > (1u << 30))
    throw FormatError("bad image dimensions");
  if (h.levels < 1 || h.levels > kMaxLevels) throw FormatError("bad level count");
  if (h.cblk < 2 || h.cblk % 2 || h.tile < h.cblk || h.tile % h.cblk) throw FormatError("bad tile/cblk");
  if (h.quant != 0 && h.transform == Transform::Independent) throw FormatError("lossy naive file is invalid");
  if (h.data_offset != kHeaderSize + index_bytes(h)) throw FormatError("bad data offset");
  return h;
}

size_t index_bytes(const PrmdHeader& h) {
  return size_t(h.levels) * kLevelEntrySize + (size_t(h.total_blocks) + 1) * 4;
}

void parse_index(const PrmdHeader& h, const uint8_t* buf, size_t n, std::vector<LevelEntry>& levels,
                 std::vector<uint32_t>& offsets) {
  if (n != index_bytes(h)) throw FormatError("index size mismatch");
  if (crc32(buf, n) != h.index_crc32) throw FormatError("index CRC mismatch");
  levels.resize(h.levels);
  const uint8_t* p = buf;
  for (auto& l : levels) {
    l.width = get32(p);
    l.height = get32(p + 4);
    l.blocks_x = get16(p + 8);
    l.blocks_y = get16(p + 10);
    l.first_block = get32(p + 12);
    p += kLevelEntrySize;
  }
  offsets.resize(size_t(h.total_blocks) + 1);
  for (auto& o : offsets) {
    o = get32(p);
    p += 4;
  }
  if (offsets[0] != 0) throw FormatError("index: first offset must be 0");
  for (size_t i = 1; i < offsets.size(); ++i)
    if (offsets[i] < offsets[i - 1]) throw FormatError("index: offsets not monotonic");
  // The level table must agree with the geometry derived from the header.
  const auto g = make_geometry(int(h.width), int(h.height), h.levels, h.tile, h.cblk, h.transform);
  uint32_t total = 0;
  for (size_t L = 0; L < levels.size(); ++L) {
    const auto& e = levels[L];
    if (int(e.width) != g[L].w || int(e.height) != g[L].h || e.blocks_x != g[L].blocks_x ||
        e.blocks_y != g[L].blocks_y || e.first_block != g[L].first_block)
      throw FormatError("level table inconsistent with header");
    total += g[L].num_ids;
  }
  if (total != h.total_blocks) throw FormatError("block count inconsistent with header");
}

}  // namespace prmd::internal
