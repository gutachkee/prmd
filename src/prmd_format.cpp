#include "prmd/internal/prmd_format.h"

#include <cstring>
#include <string>

#include "prmd/errors.h"

namespace prmd::internal {

namespace {

constexpr size_t kOffsetSize = 4;  // one uint32 per block offset

// Little-endian stores into a pre-sized buffer.
void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = uint8_t(v);
  b[at + 1] = uint8_t(v >> 8);
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[at + size_t(i)] = uint8_t(v >> (8 * i));
  }
}

void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b[at + size_t(i)] = uint8_t(v >> (8 * i));
  }
}

// Little-endian loads.
uint16_t get16(const uint8_t* p) {
  return uint16_t(p[0] | (p[1] << 8));
}

uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

uint64_t get64(const uint8_t* p) {
  return uint64_t(get32(p)) | (uint64_t(get32(p + 4)) << 32);
}

// Field-range checks on a parsed header (ids are checked while parsing).
void validate_header(const PrmdHeader& h) {
  if (h.levels < 1 || h.levels > kMaxLevels) {
    throw FormatError("bad level count");
  }
  if (const char* why =
          geometry_error(h.width, h.height, h.levels, h.tile, h.cblk)) {
    throw FormatError(std::string("bad geometry: ") + why);
  }
  // Checked before anything is sized from total_blocks, so a hostile
  // header cannot request a huge index allocation.
  const auto geom = make_geometry(int(h.width), int(h.height), h.levels, h.tile,
                                  h.cblk, h.transform);
  uint64_t total = 0;
  for (const LevelGeom& g : geom) {
    total += g.num_ids;
  }
  if (total != h.total_blocks) {
    throw FormatError("block count inconsistent with header");
  }
  if (h.quant != 0 && h.transform == Transform::Independent) {
    throw FormatError("lossy naive file is invalid");
  }
  // One encoding per file: gray has no color transform to turn off.
  if (!h.ycocg && channels(h.color) != 3) {
    throw FormatError("no-YCoCg-R flag on a non-RGB file");
  }
  if (h.data_offset != kHeaderSize + index_bytes(h)) {
    throw FormatError("bad data offset");
  }
}

// The level table must agree with the geometry derived from the header.
void check_level_table(const PrmdHeader& h,
                       const std::vector<LevelEntry>& levels) {
  const auto geom = make_geometry(int(h.width), int(h.height), h.levels, h.tile,
                                  h.cblk, h.transform);
  uint32_t total = 0;
  for (size_t level = 0; level < levels.size(); ++level) {
    const LevelEntry& e = levels[level];
    const LevelGeom& g = geom[level];
    if (int(e.width) != g.w || int(e.height) != g.h ||
        e.blocks_x != g.blocks_x || e.blocks_y != g.blocks_y ||
        e.first_block != g.first_block) {
      throw FormatError("level table inconsistent with header");
    }
    total += g.num_ids;
  }
  if (total != h.total_blocks) {
    throw FormatError("block count inconsistent with header");
  }
}

}  // namespace

const char* geometry_error(uint64_t width,
                           uint64_t height,
                           int levels,
                           int tile,
                           int cblk) {
  if (width == 0 || height == 0 || width > (1u << 30) || height > (1u << 30)) {
    return "image dimensions must be 1..2^30";
  }
  if (levels < 1 || levels > kMaxLevels) {
    return "level count must be 1..16";
  }
  if (cblk < 16 || cblk % 2 || cblk > 4096) {
    return "cblk must be even, 16..4096";
  }
  if (tile < cblk || tile % cblk || tile > 32768) {
    return "tile must be a multiple of cblk, <= 32768";
  }
  const uint64_t k = uint64_t(tile / cblk);
  uint64_t total = 0;
  uint64_t w = width;
  uint64_t h = height;
  for (int level = 0; level < levels; ++level) {
    const uint64_t bx = (w + uint64_t(cblk) - 1) / uint64_t(cblk);
    const uint64_t by = (h + uint64_t(cblk) - 1) / uint64_t(cblk);
    if (bx > 0xFFFF || by > 0xFFFF) {
      return "too many code-blocks per level row/column (max 65535)";
    }
    const uint64_t tx = (w + uint64_t(tile) - 1) / uint64_t(tile);
    const uint64_t ty = (h + uint64_t(tile) - 1) / uint64_t(tile);
    total += tx * ty * k * k;
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }
  // One extra offset entry follows the last block.
  if (total >= 0xFFFFFFFFull) {
    return "too many blocks for a 32-bit block index";
  }
  return nullptr;
}

std::vector<LevelGeom> make_geometry(int width,
                                     int height,
                                     int levels,
                                     int tile,
                                     int cblk,
                                     Transform t) {
  std::vector<LevelGeom> geom(static_cast<size_t>(levels));

  int w = width;
  int h = height;
  for (int level = 0; level < levels; ++level) {
    LevelGeom& g = geom[size_t(level)];
    g.w = w;
    g.h = h;
    g.k = tile / cblk;
    g.blocks_x = (w + cblk - 1) / cblk;
    g.blocks_y = (h + cblk - 1) / cblk;
    g.tiles_x = (w + tile - 1) / tile;
    g.tiles_y = (h + tile - 1) / tile;
    g.num_ids = uint32_t(uint64_t(g.tiles_x) * uint64_t(g.tiles_y) *
                         uint64_t(g.k) * uint64_t(g.k));
    g.base = (level == levels - 1) || t == Transform::Independent;
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }

  // Data order: coarsest level first.
  uint32_t next = 0;
  for (int level = levels - 1; level >= 0; --level) {
    geom[size_t(level)].first_block = next;
    next += geom[size_t(level)].num_ids;
  }
  return geom;
}

Rect block_pixel_rect(const LevelGeom& g, int cblk, int bx, int by) {
  return intersect(Rect{bx * cblk, by * cblk, cblk, cblk},
                   Rect{0, 0, g.w, g.h});
}

std::vector<uint8_t> serialize(PrmdHeader& h,
                               const std::vector<LevelEntry>& levels,
                               const std::vector<uint32_t>& offsets) {
  const size_t index_size =
      levels.size() * kLevelEntrySize + offsets.size() * kOffsetSize;
  std::vector<uint8_t> b(kHeaderSize + index_size, 0);

  // Index: level table, then block offsets.
  size_t at = kHeaderSize;
  for (const LevelEntry& l : levels) {
    put32(b, at, l.width);
    put32(b, at + 4, l.height);
    put16(b, at + 8, l.blocks_x);
    put16(b, at + 10, l.blocks_y);
    put32(b, at + 12, l.first_block);
    at += kLevelEntrySize;
  }
  for (uint32_t o : offsets) {
    put32(b, at, o);
    at += kOffsetSize;
  }

  h.data_offset = kHeaderSize + index_size;

  // Fixed header.
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
  b[28] = uint8_t(h.entropy);
  b[29] = h.entropy_level;
  b[30] = uint8_t((h.med ? 0 : kFlagNoMed) | (h.ycocg ? 0 : kFlagNoYcocg));
  put64(b, 32, h.data_offset);
  return b;
}

PrmdHeader parse_header(const uint8_t* p, size_t n) {
  if (n < kHeaderSize || std::memcmp(p, kMagic, 4) != 0) {
    throw FormatError("not a .prmd file (bad magic)");
  }

  PrmdHeader h;
  h.version = get16(p + 4);
  if (h.version != kVersion) {
    throw FormatError("unsupported .prmd version " + std::to_string(h.version));
  }

  if (p[6] > uint8_t(Transform::Independent)) {
    throw FormatError("unknown transform id");
  }
  h.transform = Transform(p[6]);

  if (p[7] > uint8_t(Downsampler::Gauss)) {
    throw FormatError("unknown downsampler id");
  }
  h.downsampler = Downsampler(p[7]);

  h.width = get32(p + 8);
  h.height = get32(p + 12);
  h.levels = p[16];

  if (!is_valid_color_format(p[17])) {
    throw FormatError("unknown color format id");
  }
  h.color = ColorFormat(p[17]);

  h.tile = get16(p + 18);
  h.cblk = get16(p + 20);
  h.quant = get16(p + 22);
  h.total_blocks = get32(p + 24);
  if (p[28] > uint8_t(Entropy::Raw)) {
    throw FormatError("unknown entropy coder id");
  }
  h.entropy = Entropy(p[28]);
  h.entropy_level = p[29];
  if (p[30] & ~(kFlagNoMed | kFlagNoYcocg)) {
    throw FormatError("unknown header flags");
  }
  h.med = !(p[30] & kFlagNoMed);
  h.ycocg = !(p[30] & kFlagNoYcocg);
  h.data_offset = get64(p + 32);

  // Reserved bytes must be zero, so a future version can give them meaning.
  if (p[31] != 0) {
    throw FormatError("reserved header bytes are not zero");
  }
  for (size_t i = 40; i < kHeaderSize; ++i) {
    if (p[i] != 0) {
      throw FormatError("reserved header bytes are not zero");
    }
  }

  validate_header(h);
  return h;
}

size_t index_bytes(const PrmdHeader& h) {
  return size_t(h.levels) * kLevelEntrySize +
         (size_t(h.total_blocks) + 1) * kOffsetSize;
}

void parse_index(const PrmdHeader& h,
                 const uint8_t* buf,
                 size_t n,
                 std::vector<LevelEntry>& levels,
                 std::vector<uint32_t>& offsets) {
  if (n != index_bytes(h)) {
    throw FormatError("index size mismatch");
  }
  const uint8_t* p = buf;
  levels.resize(h.levels);
  for (LevelEntry& l : levels) {
    l.width = get32(p);
    l.height = get32(p + 4);
    l.blocks_x = get16(p + 8);
    l.blocks_y = get16(p + 10);
    l.first_block = get32(p + 12);
    p += kLevelEntrySize;
  }

  offsets.resize(size_t(h.total_blocks) + 1);
  for (uint32_t& o : offsets) {
    o = get32(p);
    p += kOffsetSize;
  }

  if (offsets[0] != 0) {
    throw FormatError("index: first offset must be 0");
  }
  for (size_t i = 1; i < offsets.size(); ++i) {
    if (offsets[i] < offsets[i - 1]) {
      throw FormatError("index: offsets not monotonic");
    }
  }

  check_level_table(h, levels);
}

}  // namespace prmd::internal
