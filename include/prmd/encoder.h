#pragma once
// Encoding entry point.

#include <array>
#include <cstdint>
#include <string>

#include "prmd/image.h"

namespace prmd {

constexpr int kMaxLevels = 16;

enum class Codec : uint8_t { Prmd, J2K };
// Values are stable on-disk ids.
enum class Transform : uint8_t { Haar = 0, Cdf53 = 1, Laplacian = 2, Independent = 3 };  // Independent = naive
enum class Downsampler : uint8_t { Box = 0, Gauss = 1 };  // used only by Laplacian and Independent
// How code-block numbers become bytes. Values are stable on-disk ids.
enum class Entropy : uint8_t {
  Range = 0,  // custom context-adaptive binary range coder: best ratio, ~10x slower decode
  Zstd = 1,   // zigzag int16 byte planes + zstd: default — fast decode, a few % larger
  Raw = 2,    // zigzag int16, uncompressed: no entropy coding, for measurement only
};

const char* to_string(Transform t);
const char* to_string(Downsampler d);
const char* to_string(Entropy e);

struct EncodeOptions {
  int levels = 5;  // N, 1..kMaxLevels
  ColorFormat color = ColorFormat::Gray8;  // stored format; the source is converted if it differs
  // .prmd
  Transform transform = Transform::Haar;
  Downsampler downsampler = Downsampler::Box;
  int tile = 256;   // reporting/grouping unit; multiple of cblk
  int cblk = 128;   // code-block = independently decodable unit; even, >= 16
  int quant = 0;    // 0 = lossless; Q > 0 = lossy quantizer step (haar/cdf53/laplacian only)
  Entropy entropy = Entropy::Zstd;  // default: see DESIGN.md §7.1 (decision record)
  int entropy_level = 0;  // 0 = coder default. Zstd: 1..22 (default 19). Range, Raw: must be 0.
  // J2K
  int j2k_tile = 256;
  int j2k_cblk = 64;
  bool j2k_lossless = true;
  float j2k_psnr = 0.f;
};

struct EncodeReport {
  uint64_t file_bytes = 0;
  std::array<uint64_t, kMaxLevels> level_bytes{};  // payload bytes attributed to each level
  double encode_ms = 0;
};

// Throws std::invalid_argument for bad options, prmd::IoError on write failure.
EncodeReport encode_file(const Image& src, const std::string& out_path, Codec codec,
                         const EncodeOptions& opts);

}  // namespace prmd
