#include "prmd/pyramid.h"

#include <stdexcept>

#include "prmd/internal/planes.h"
#include "prmd/internal/transform.h"

namespace prmd {
namespace internal {

std::vector<Region> to_planes(const Image& img) {
  const int ch = img.channels();
  std::vector<Region> planes(size_t(ch), Region(img.bounds()));
  for (int y = 0; y < img.height; ++y) {
    const uint8_t* src = img.row(y);
    for (int c = 0; c < ch; ++c) {
      int32_t* dst = planes[size_t(c)].row(y);
      for (int x = 0; x < img.width; ++x) dst[x] = src[size_t(x) * size_t(ch) + size_t(c)];
    }
  }
  return planes;
}

Image from_planes(const std::vector<Region>& planes, ColorFormat f) {
  const int ch = channels(f);
  if (int(planes.size()) != ch) throw std::invalid_argument("from_planes: channel mismatch");
  const Rect r = planes[0].rect;
  Image img(r.w, r.h, f);
  for (int y = 0; y < r.h; ++y) {
    uint8_t* dst = img.row(y);
    for (int c = 0; c < ch; ++c) {
      const int32_t* src = planes[size_t(c)].row(r.y + y);
      for (int x = 0; x < r.w; ++x) {
        const int32_t v = src[x];
        dst[size_t(x) * size_t(ch) + size_t(c)] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
      }
    }
  }
  return img;
}

std::vector<std::vector<Region>> build_level_planes(const Image& src, int levels, Transform t,
                                                    Downsampler d) {
  if (levels < 1 || levels > kMaxLevels) throw std::invalid_argument("levels must be in 1..16");
  std::vector<std::vector<Region>> out;
  out.push_back(to_planes(src));
  std::unique_ptr<LevelTransform> xf;
  if (t != Transform::Independent) xf = make_transform(t, d);
  for (int L = 1; L < levels; ++L) {
    std::vector<Region> next;
    for (const Region& fine : out.back()) {
      Region coarse;
      if (xf) {
        std::vector<Region> bands;
        xf->forward(fine, coarse, bands);
      } else {
        coarse = d == Downsampler::Gauss ? downsample_gauss(fine) : downsample_box(fine);
      }
      next.push_back(std::move(coarse));
    }
    out.push_back(std::move(next));
  }
  return out;
}

}  // namespace internal

std::vector<Image> build_pyramid(const Image& src, int levels, Transform t, Downsampler d) {
  const auto planes = internal::build_level_planes(src, levels, t, d);
  std::vector<Image> out;
  for (const auto& lv : planes) out.push_back(internal::from_planes(lv, src.format));
  return out;
}

}  // namespace prmd
