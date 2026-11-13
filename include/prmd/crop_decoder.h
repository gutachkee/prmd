#pragma once
// The common decoding interface shared by every codec.

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "prmd/encoder.h"
#include "prmd/image.h"

namespace prmd {

struct LevelInfo {
  int width = 0, height = 0;
};

// Statistics for one decode() call. Every codec fills these in the same way.
struct DecodeStats {
  int tiles_touched = 0;   // distinct (level, tile) pairs whose blocks were used
                           //   (J2K: distinct J2K tiles intersecting the decode window)
  int blocks_decoded = 0;  // independently entropy-coded units decoded, all levels
  std::array<int, kMaxLevels> blocks_per_level{};  // the same count, by level
  uint64_t bytes_read = 0;  // file bytes fetched by this call (excludes open())
  int read_calls = 0;       // number of pread()/stream reads issued
  int cache_hits = 0;       // blocks served from the LRU cache (0 when the cache is off)
};

// What decode() returns: the pixels, the rect they cover (== the request for decode(),
// the clamped intersection for decode_clamped()), and the cost of producing them.
struct DecodeResult {
  Image image;
  Rect rect;
  DecodeStats stats;
};

struct DecoderOptions {
  size_t cache_bytes = 0;  // LRU budget for decoded blocks; 0 = disabled
};

class CropDecoder {
 public:
  virtual ~CropDecoder() = default;

  virtual std::string name() const = 0;  // e.g. "prmd/haar", "j2k"
  virtual int num_levels() const = 0;
  virtual ColorFormat color_format() const = 0;  // as encoded; decode() returns this format
  virtual LevelInfo level_info(int level) const = 0;
  virtual uint64_t file_bytes() const = 0;
  virtual uint64_t open_bytes_read() const = 0;  // header + index read at open

  // Strict: requires 0 <= level < num_levels(), r.w > 0, r.h > 0, r.x >= 0, r.y >= 0,
  // r.right() <= width(level), r.bottom() <= height(level). Otherwise throws std::out_of_range.
  DecodeResult decode(int level, const Rect& r);

  // Clamping variant: decodes the intersection of r with the level (DecodeResult::rect).
  // Throws std::out_of_range if the intersection is empty or the level is invalid.
  DecodeResult decode_clamped(int level, const Rect& r);

 protected:
  // Codec-specific work. Called only with a valid level and a non-empty in-bounds rect.
  virtual Image decode_region(int level, const Rect& r, DecodeStats& stats) = 0;
};

// Picks the codec by file magic: "PRMD" → .prmd, J2K codestream (FF 4F FF 51) → JPEG2000.
// Throws prmd::FormatError for unknown or corrupt files, prmd::IoError if unreadable.
std::unique_ptr<CropDecoder> open_decoder(const std::string& path, const DecoderOptions& opts = {});

}  // namespace prmd
