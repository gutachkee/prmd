#pragma once
// Code-block payload coding: pluggable entropy coders, YCoCg-R, MED prediction.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "prmd/encoder.h"
#include "prmd/internal/region.h"
#include "prmd/internal/transform.h"

namespace prmd::internal {

// One code-block = (bands) × (channels) planes of signed ints with identical
// dims. Plane order: for each band, for each channel (e.g. RGB haar: LH.Y LH.Co
// LH.Cg HL.Y …). For RGB8 the planes hold YCoCg-R values; channel 0 (luma)
// and 1..2 (chroma) use separate context sets in the range coder.
struct BlockPlanes {
  int w = 0, h = 0;
  int channels = 1;
  std::vector<Band> kinds;                   // one per plane
  std::vector<Samples> planes;  // each w*h, row-major
};

// Pluggable entropy coder for one code-block. All adaptive state is created
// fresh per call, so every block is independently decodable. Thread-safe
// (const, no shared state).
class BlockCoder {
 public:
  virtual ~BlockCoder() = default;
  virtual Entropy id() const = 0;
  virtual void encode(const BlockPlanes& in,
                      std::vector<uint8_t>& out) const = 0;
  // `io` arrives with w, h, channels and kinds filled in; planes get resized
  // and filled. Throws FormatError on a truncated or inconsistent payload.
  virtual void decode(const uint8_t* data,
                      size_t size,
                      BlockPlanes& io) const = 0;
};

// level: EncodeOptions::entropy_level (0 = default). Throws
// std::invalid_argument for an unknown id or invalid level.
std::unique_ptr<BlockCoder> make_block_coder(Entropy e, int level = 0);

// Reversible color decorrelation (YCoCg-R), in place, on any signed ints.
void ycocg_r_forward(int32_t* c0, int32_t* c1, int32_t* c2, size_t n);
void ycocg_r_inverse(int32_t* c0, int32_t* c1, int32_t* c2, size_t n);

// MED (LOCO-I) prediction, causal within the w×h block only. First sample
// predicted as 0.
void med_residual(const int32_t* px, int w, int h, int32_t* res);
void med_reconstruct(const int32_t* res, int w, int h, int32_t* px);

}  // namespace prmd::internal
