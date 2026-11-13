#pragma once
// .prmd on-disk structures (DESIGN.md §3). All integers little-endian, explicit (de)serialization.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "prmd/encoder.h"
#include "prmd/image.h"

namespace prmd::internal {

constexpr char kMagic[4] = {'P', 'R', 'M', 'D'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 64;
constexpr size_t kLevelEntrySize = 16;

struct PrmdHeader {
  uint16_t version = kVersion;
  Transform transform = Transform::Haar;
  Downsampler downsampler = Downsampler::Box;
  uint32_t width = 0, height = 0;
  uint8_t levels = 0;
  ColorFormat color = ColorFormat::Gray8;
  uint16_t tile = 256, cblk = 128, quant = 0;
  uint32_t total_blocks = 0;
  uint32_t index_crc32 = 0;
  uint64_t data_offset = 0;
  Entropy entropy = Entropy::Range;
  uint8_t entropy_level = 0;
};

struct LevelEntry {
  uint32_t width = 0, height = 0;
  uint16_t blocks_x = 0, blocks_y = 0;
  uint32_t first_block = 0;
};

// Derived per-level geometry shared by encoder and decoder.
struct LevelGeom {
  int w = 0, h = 0;
  int blocks_x = 0, blocks_y = 0;  // code-blocks
  int tiles_x = 0, tiles_y = 0;    // tiles (each k×k blocks)
  int k = 1;                       // tile / cblk
  uint32_t first_block = 0;        // first block id of this level
  uint32_t num_ids = 0;            // tiles_x*tiles_y*k*k (ids of absent edge blocks are empty)
  bool base = false;               // stored directly (MED) rather than as detail/residual
};

// Level geometry for a whole file. Level N-1 is first in the data section, level 0 last.
std::vector<LevelGeom> make_geometry(int width, int height, int levels, int tile, int cblk,
                                     Transform t);

// Tile-major id: first + (ty*tiles_x + tx)*k² + (by%k)*k + (bx%k).
inline uint32_t block_id(const LevelGeom& g, int bx, int by) {
  const int tx = bx / g.k, ty = by / g.k;
  return g.first_block + uint32_t((ty * g.tiles_x + tx) * g.k * g.k + (by % g.k) * g.k + (bx % g.k));
}
inline int tile_index(const LevelGeom& g, int bx, int by) { return (by / g.k) * g.tiles_x + bx / g.k; }

// Pixel rect of block (bx, by) in its level, clipped to the level.
Rect block_pixel_rect(const LevelGeom& g, int cblk, int bx, int by);

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

// Serializes header + level table + offsets (header.index_crc32 / data_offset are filled in).
std::vector<uint8_t> serialize(PrmdHeader& h, const std::vector<LevelEntry>& levels,
                               const std::vector<uint32_t>& offsets);

// Parses the fixed header. Throws FormatError on bad magic/version/field values.
PrmdHeader parse_header(const uint8_t* buf, size_t n);
// Size of level table + offsets that follow the header.
size_t index_bytes(const PrmdHeader& h);
// Parses level table + offsets; validates CRC, monotonic offsets and geometry consistency.
void parse_index(const PrmdHeader& h, const uint8_t* buf, size_t n, std::vector<LevelEntry>& levels,
                 std::vector<uint32_t>& offsets);

}  // namespace prmd::internal
