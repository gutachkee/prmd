#pragma once
// Per-mode level transform: how level L is split into level L+1 plus
// detail/residual bands, and how a region of level L is rebuilt from regions of
// those.
//
// All planes are int32 and addressed in their own plane coordinates (origin
// 0,0). Fine level dims (fw, fh) are the true level dims; Haar and Cdf53
// internally pad to even dims by replicating the last column/row.

#include <memory>
#include <vector>

#include "prmd/encoder.h"
#include "prmd/internal/region.h"

namespace prmd::internal {

enum class Band : uint8_t { Base = 0, LH = 1, HL = 2, HH = 3, Residual = 4 };
constexpr int kNumBandKinds = 5;

struct Dims {
  int w = 0;
  int h = 0;
};
inline Dims half_up(Dims d) {
  return Dims{(d.w + 1) / 2, (d.h + 1) / 2};
}

class LevelTransform {
 public:
  virtual ~LevelTransform() = default;

  virtual int num_bands() const = 0;  // 3 (Haar, Cdf53) or 1 (Laplacian)
  virtual Band band_kind(int i) const = 0;
  // Band plane dims for a fine level of dims `fine`.
  virtual Dims band_size(Dims fine) const = 0;
  // Band-plane rect covered by the fine-pixel rect `fine_r` (used to map
  // code-blocks, which are defined in fine pixels, to band samples).
  virtual Rect band_rect_of_block(const Rect& fine_r, Dims fine) const = 0;

  // Encoder: from a full fine level, produce the full coarse level (dims
  // half_up(fine)) and the full bands (dims band_size(fine)).
  virtual void forward(const Region& fine,
                       Region& coarse,
                       std::vector<Region>& bands) const = 0;

  // Decoder: coarse / band samples needed to reconstruct fine rect r, clipped
  // to the planes.
  virtual Rect coarse_needed(const Rect& fine_r, Dims fine) const = 0;
  virtual Rect band_needed(const Rect& fine_r, Dims fine) const = 0;

  // Decoder: reconstruct exactly fine_r into `out` (out.rect == fine_r on
  // return) from regions that cover coarse_needed / band_needed.
  virtual void inverse(const Region& coarse,
                       const std::vector<Region>& bands,
                       const Rect& fine_r,
                       Dims fine,
                       Region& out) const = 0;
};

// ----------------------------------------------------------------------------
// Lossy quantization

// Uniform rounding quantizer: idx = sign(v)·⌊(|v| + step/2) / step⌋, value =
// idx·step, so |v − value| ≤ step/2. step <= 1 means lossless (identity).
inline int32_t quantize(int32_t v, int step) {
  if (step <= 1) {
    return v;
  }
  const int32_t h = step / 2;
  return v >= 0 ? (v + h) / step : -((-v + h) / step);
}
inline int32_t dequantize(int32_t idx, int step) {
  return step <= 1 ? idx : idx * step;
}

// Step for a band given the file's quant parameter q (0 = lossless). Haar/Cdf53
// HH errors reach the pixels at half amplitude (synthesis gain ¼ in energy vs
// LH/HL), so HH uses 2q for equal distortion per band. Base (coarsest level) is
// always lossless.
inline int band_step(Band b, int q) {
  if (q <= 0 || b == Band::Base) {
    return 0;
  }
  return b == Band::HH ? 2 * q : q;
}

// Transform::Independent has no LevelTransform (every level is stored as Base);
// throws std::invalid_argument for it.
std::unique_ptr<LevelTransform> make_transform(Transform t, Downsampler d);

// Downsamplers used by Laplacian (D) and Independent. Output dims =
// half_up(input dims).

// (a+b+c+d+2)>>2, last row/col replicated.
Region downsample_box(const Region& fine);

// [1 4 6 4 1]/16 separable, reflect-101, (s+128)>>8.
Region downsample_gauss(const Region& fine);

// Dispatches to downsample_box or downsample_gauss.
Region downsample(const Region& fine, Downsampler d);

}  // namespace prmd::internal
