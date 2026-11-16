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
  int width = 0;
  int height = 0;
};

// Statistics for one decode() call. Every codec fills these in the same way.
struct DecodeStats {
  // Distinct (level, tile) pairs whose blocks were used (J2K: distinct J2K
  // tiles intersecting the decode window).
  int tiles_touched = 0;
  // Independently entropy-coded units decoded, all levels.
  int blocks_decoded = 0;
  // The same count, by level.
  std::array<int, kMaxLevels> blocks_per_level{};
  // File bytes fetched by this call (excludes open()).
  uint64_t bytes_read = 0;
  // Number of pread()/stream reads issued.
  int read_calls = 0;
  // Blocks served from the LRU cache (0 when the cache is off).
  int cache_hits = 0;
};

// What decode() returns: the pixels, the rect they cover (== the request for
// decode(), the clamped intersection for decode_clamped()), and the cost of
// producing them.
struct DecodeResult {
  Image image;
  Rect rect;
  DecodeStats stats;
};

struct DecoderOptions {
  size_t cache_bytes = 0;  // LRU budget for decoded blocks; 0 = disabled
};

// Not thread-safe: decode() reads the file and updates the block cache
// without locking. Use one decoder per thread, or serialize calls.
class CropDecoder {
 public:
  virtual ~CropDecoder() = default;

  virtual std::string name() const = 0;  // e.g. "prmd/haar", "j2k"
  virtual int num_levels() const = 0;
  // As encoded; decode() returns this format.
  virtual ColorFormat color_format() const = 0;
  virtual LevelInfo level_info(int level) const = 0;
  virtual uint64_t file_bytes() const = 0;
  // Header + index read at open.
  virtual uint64_t open_bytes_read() const = 0;

  // Strict: requires 0 <= level < num_levels(), r.w > 0, r.h > 0, r.x >= 0,
  // r.y >= 0, r.x + r.w <= width(level), r.y + r.h <= height(level)
  // (checked without int overflow).
  // Otherwise throws std::out_of_range.
  DecodeResult decode(int level, const Rect& r);

  // Clamping variant: decodes the intersection of r with the level
  // (DecodeResult::rect). Throws std::out_of_range if the intersection is
  // empty or the level is invalid.
  DecodeResult decode_clamped(int level, const Rect& r);

 protected:
  // Codec-specific work. Called only with a valid level and a non-empty
  // in-bounds rect.
  virtual Image decode_region(int level, const Rect& r, DecodeStats& stats) = 0;

 private:
  // Throws std::out_of_range unless 0 <= level < num_levels().
  void check_level(int level, const Rect& r) const;
};

// Picks the codec by file magic: "PRMD" → .prmd, J2K codestream (FF 4F FF 51)
// → JPEG2000. Throws prmd::FormatError for unknown or corrupt files,
// prmd::IoError if unreadable.
std::unique_ptr<CropDecoder> open_decoder(const std::string& path,
                                          const DecoderOptions& opts = {});

}  // namespace prmd
