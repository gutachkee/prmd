#pragma once
// Independent reference pyramid builders for tests. Written deliberately differently from the
// codec (plain 2D int arrays, explicit extension, explicit floor division, 2D kernels) so
// that a shared bug is unlikely.

#include <vector>

#include "prmd/encoder.h"
#include "prmd/image.h"

namespace ref {

std::vector<prmd::Image> pyramid(const prmd::Image& src, int levels, prmd::Transform t,
                                 prmd::Downsampler d);

// Random-ish test image with smooth regions, edges and noise (more realistic than white noise).
prmd::Image synthetic_image(int w, int h, prmd::ColorFormat f, unsigned seed);

}  // namespace ref
