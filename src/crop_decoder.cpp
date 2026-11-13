#include "prmd/crop_decoder.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "prmd/errors.h"
#include "prmd/internal/j2k_codec.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"

namespace prmd {

namespace {
std::string describe(int level, const Rect& r) {
  return "level " + std::to_string(level) + " rect {" + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
         std::to_string(r.w) + "," + std::to_string(r.h) + "}";
}
}  // namespace

DecodeResult CropDecoder::decode(int level, const Rect& r) {
  if (level < 0 || level >= num_levels()) throw std::out_of_range("invalid " + describe(level, r));
  const LevelInfo li = level_info(level);
  if (r.empty() || r.x < 0 || r.y < 0 || r.right() > li.width || r.bottom() > li.height)
    throw std::out_of_range("out of bounds: " + describe(level, r) + " (level is " + std::to_string(li.width) +
                            "x" + std::to_string(li.height) + ")");
  DecodeResult res;
  res.rect = r;
  res.image = decode_region(level, r, res.stats);
  return res;
}

DecodeResult CropDecoder::decode_clamped(int level, const Rect& r) {
  if (level < 0 || level >= num_levels()) throw std::out_of_range("invalid " + describe(level, r));
  const LevelInfo li = level_info(level);
  const Rect c = intersect(r, Rect{0, 0, li.width, li.height});
  if (c.empty()) throw std::out_of_range("no overlap with the level: " + describe(level, r));
  return decode(level, c);
}

std::unique_ptr<CropDecoder> open_decoder(const std::string& path, const DecoderOptions& opts) {
  unsigned char sig[4] = {0};
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) throw IoError("cannot open '" + path + "'");
  const size_t n = std::fread(sig, 1, 4, f);
  std::fclose(f);
  if (n == 4 && std::memcmp(sig, internal::kMagic, 4) == 0) return internal::open_prmd(path, opts);
  if (n == 4 && sig[0] == 0xFF && sig[1] == 0x4F && sig[2] == 0xFF && sig[3] == 0x51)
    return internal::open_j2k(path, opts);
  throw FormatError("unrecognized file format: '" + path + "'");
}

}  // namespace prmd
