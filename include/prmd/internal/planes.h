#pragma once
// Conversions between Image and per-channel int32 planes, and the integer level
// chain.

#include <vector>

#include "prmd/encoder.h"
#include "prmd/image.h"
#include "prmd/internal/region.h"

namespace prmd::internal {

// One whole-plane Region per channel.
std::vector<Region> to_planes(const Image& img);
// Clamps to [0, 255]. All planes must share one rect; returns an image of that
// rect's size.
Image from_planes(const std::vector<Region>& planes, ColorFormat f);

// Integer level planes [level][channel], level 0 = the image. Each level is
// derived from the previous one's unclamped integers (relevant only for Cdf53,
// whose LL can leave [0, 255]).
std::vector<std::vector<Region>> build_level_planes(const Image& src,
                                                    int levels,
                                                    Transform t,
                                                    Downsampler d);

}  // namespace prmd::internal
