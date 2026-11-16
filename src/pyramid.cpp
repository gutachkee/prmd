#include "prmd/pyramid.h"

#include <stdexcept>

#include "prmd/internal/parallel.h"
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
      for (int x = 0; x < img.width; ++x) {
        dst[x] = src[size_t(x) * size_t(ch) + size_t(c)];
      }
    }
  }
  return planes;
}

std::vector<Region> to_planes(const Image& img,
                              ColorFormat stored,
                              Pool* pool) {
  const bool to_gray =
      img.format == ColorFormat::RGB8 && stored == ColorFormat::Gray8;
  const bool to_rgb =
      img.format == ColorFormat::Gray8 && stored == ColorFormat::RGB8;
  if (img.format != stored && !to_gray && !to_rgb) {
    throw std::invalid_argument("convert: unsupported format pair");
  }
  const int in_ch = img.channels();
  const int out_ch = channels(stored);
  // Every sample is written below, by the thread that owns its row.
  std::vector<Region> planes;
  for (int c = 0; c < out_ch; ++c) {
    planes.emplace_back(img.bounds(), kNoInit);
  }
  const size_t w = size_t(img.width);

  const size_t row_samples = w * size_t(in_ch + out_ch);
  parallel_rows(pool, img.height, row_samples, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
      const uint8_t* src = img.row(y);
      if (img.format == stored) {
        for (int c = 0; c < out_ch; ++c) {
          int32_t* dst = planes[size_t(c)].row(y);
          for (size_t x = 0; x < w; ++x) {
            dst[x] = src[x * size_t(in_ch) + size_t(c)];
          }
        }
      } else if (to_gray) {
        // convert(): BT.601 luma, rounded.
        int32_t* dst = planes[0].row(y);
        for (size_t x = 0; x < w; ++x) {
          const uint8_t* s = src + 3 * x;
          dst[x] = uint8_t((77u * s[0] + 150u * s[1] + 29u * s[2] + 128u) >> 8);
        }
      } else {  // to_rgb: gray replicated into R, G, B
        for (int c = 0; c < 3; ++c) {
          int32_t* dst = planes[size_t(c)].row(y);
          for (size_t x = 0; x < w; ++x) {
            dst[x] = src[x];
          }
        }
      }
    }
  });
  return planes;
}

Image from_planes(const std::vector<Region>& planes, ColorFormat f) {
  const int ch = channels(f);
  if (int(planes.size()) != ch) {
    throw std::invalid_argument("from_planes: channel mismatch");
  }

  const Rect r = planes[0].rect;
  Image img(r.w, r.h, f);
  for (int y = 0; y < r.h; ++y) {
    uint8_t* dst = img.row(y);
    for (int c = 0; c < ch; ++c) {
      const int32_t* src = planes[size_t(c)].row(r.y + y);
      for (int x = 0; x < r.w; ++x) {
        dst[size_t(x) * size_t(ch) + size_t(c)] = clamp_to_u8(src[x]);
      }
    }
  }
  return img;
}

std::vector<std::vector<Region>> build_level_planes(const Image& src,
                                                    int levels,
                                                    Transform t,
                                                    Downsampler d) {
  if (levels < 1 || levels > kMaxLevels) {
    throw std::invalid_argument("levels must be in 1..16");
  }

  std::unique_ptr<LevelTransform> xf;
  if (t != Transform::Independent) {
    xf = make_transform(t, d);
  }

  std::vector<std::vector<Region>> out;
  out.push_back(to_planes(src));
  for (int level = 1; level < levels; ++level) {
    std::vector<Region> next;
    for (const Region& fine : out.back()) {
      Region coarse;
      if (xf) {
        std::vector<Region> unused_bands;
        xf->forward(fine, coarse, unused_bands);
      } else {
        coarse = downsample(fine, d);
      }
      next.push_back(std::move(coarse));
    }
    out.push_back(std::move(next));
  }
  return out;
}

}  // namespace internal

std::vector<Image> build_pyramid(const Image& src,
                                 int levels,
                                 Transform t,
                                 Downsampler d) {
  const auto planes = internal::build_level_planes(src, levels, t, d);
  std::vector<Image> out;
  for (const auto& level_planes : planes) {
    out.push_back(internal::from_planes(level_planes, src.format));
  }
  return out;
}

}  // namespace prmd
