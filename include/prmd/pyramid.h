#pragma once
// Pyramid level definitions. build_pyramid() is the normative definition of every level for
// each mode; the codec's decoded crops must equal crops of these images exactly (lossless).

#include <vector>

#include "prmd/encoder.h"
#include "prmd/image.h"

namespace prmd {

// Returns `levels` images; [0] is `src` itself, [L] has dims ceil(W/2^L) × ceil(H/2^L).
// RGB8: every filter is applied per channel independently.
//   Haar:        LL = ⌊(⌊(a+b)/2⌋ + ⌊(c+d)/2⌋)/2⌋ (odd dims: last row/col replicated)
//   Cdf53:       5/3 LL band of the previous level's *unclamped* integer LL; each returned level
//                is that LL clamped to [0, 255]
//   Laplacian/Independent: Box (a+b+c+d+2)>>2, or Gauss [1 4 6 4 1]/16 reflect-101 (s+128)>>8
// Throws std::invalid_argument if levels is outside 1..kMaxLevels.
std::vector<Image> build_pyramid(const Image& src, int levels, Transform t, Downsampler d);

}  // namespace prmd
