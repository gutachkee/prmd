#include "prmd/crop_decoder.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "prmd/errors.h"
#include "prmd/internal/j2k_codec.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/profile.h"

namespace prmd {

namespace {

constexpr size_t kSignatureSize = 4;
constexpr unsigned char kJ2kSignature[kSignatureSize] = {0xFF, 0x4F, 0xFF,
                                                         0x51};

std::string describe(int level, const Rect& r) {
  return "level " + std::to_string(level) + " rect {" + std::to_string(r.x) +
         "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," +
         std::to_string(r.h) + "}";
}

}  // namespace

void CropDecoder::check_level(int level, const Rect& r) const {
  if (level < 0 || level >= num_levels()) {
    throw std::out_of_range("invalid " + describe(level, r));
  }
}

DecodeResult CropDecoder::decode(int level, const Rect& r) {
  PRMD_TIMED("decode");
  check_level(level, r);

  const LevelInfo li = level_info(level);
  // Compare without adding x + w, which could overflow int.
  if (r.empty() || r.x < 0 || r.y < 0 || r.w > li.width - r.x ||
      r.h > li.height - r.y) {
    throw std::out_of_range("out of bounds: " + describe(level, r) +
                            " (level is " + std::to_string(li.width) + "x" +
                            std::to_string(li.height) + ")");
  }

  DecodeResult res;
  res.rect = r;
  res.image = decode_region(level, r, res.stats);
  return res;
}

DecodeResult CropDecoder::decode_clamped(int level, const Rect& r) {
  check_level(level, r);

  const LevelInfo li = level_info(level);
  const Rect clamped = intersect(r, Rect{0, 0, li.width, li.height});
  if (clamped.empty()) {
    throw std::out_of_range("no overlap with the level: " + describe(level, r));
  }
  return decode(level, clamped);
}

std::unique_ptr<CropDecoder> open_decoder(const std::string& path,
                                          const DecoderOptions& opts) {
  PRMD_TIMED("open_decoder");
  unsigned char sig[kSignatureSize] = {0};
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    throw IoError("cannot open '" + path + "'");
  }
  const size_t n = std::fread(sig, 1, kSignatureSize, f);
  std::fclose(f);

  if (n == kSignatureSize) {
    if (std::memcmp(sig, internal::kMagic, kSignatureSize) == 0) {
      return internal::open_prmd(path, opts);
    }
    if (std::memcmp(sig, kJ2kSignature, kSignatureSize) == 0) {
      return internal::open_j2k(path, opts);
    }
  }
  throw FormatError("unrecognized file format: '" + path + "'");
}

}  // namespace prmd
