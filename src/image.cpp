#include "prmd/image.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace prmd {

namespace {

constexpr ColorFormatInfo kFormats[] = {
    {ColorFormat::Gray8, "gray", 1},
    {ColorFormat::RGB8, "rgb", 3},
};

const ColorFormatInfo* find_format(uint8_t id) {
  for (const auto& format : kFormats) {
    if (uint8_t(format.format) == id) {
      return &format;
    }
  }
  return nullptr;
}

}  // namespace

Rect intersect(const Rect& a, const Rect& b) {
  const int x0 = std::max(a.x, b.x);
  const int y0 = std::max(a.y, b.y);
  const int x1 = std::min(a.right(), b.right());
  const int y1 = std::min(a.bottom(), b.bottom());
  if (x1 <= x0 || y1 <= y0) {
    return Rect{};
  }
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

const ColorFormatInfo& info(ColorFormat f) {
  if (const ColorFormatInfo* format = find_format(uint8_t(f))) {
    return *format;
  }
  throw std::invalid_argument("unknown ColorFormat id " +
                              std::to_string(int(f)));
}

bool is_valid_color_format(uint8_t id) {
  return find_format(id) != nullptr;
}

Image::Image(int w, int h, ColorFormat f) : width(w), height(h), format(f) {
  if (w < 0 || h < 0) {
    throw std::invalid_argument("negative image size");
  }
  pixels.assign(size_t(w) * size_t(h) * size_t(prmd::channels(f)), 0);
}

Image Image::crop(const Rect& r) const {
  if (r.empty() || r.x < 0 || r.y < 0 || r.right() > width ||
      r.bottom() > height) {
    throw std::out_of_range("Image::crop: rect outside image");
  }

  Image out(r.w, r.h, format);
  const size_t c = size_t(channels());
  for (int y = 0; y < r.h; ++y) {
    std::memcpy(out.row(y), row(r.y + y) + size_t(r.x) * c, size_t(r.w) * c);
  }
  return out;
}

Image convert(const Image& src, ColorFormat to) {
  if (src.format == to) {
    return src;
  }

  Image out(src.width, src.height, to);
  const size_t n = size_t(src.width) * size_t(src.height);
  const uint8_t* s = src.pixels.data();
  uint8_t* d = out.pixels.data();

  if (src.format == ColorFormat::RGB8 && to == ColorFormat::Gray8) {
    for (size_t i = 0; i < n; ++i, s += 3) {
      d[i] = uint8_t((77u * s[0] + 150u * s[1] + 29u * s[2] + 128u) >> 8);
    }
  } else if (src.format == ColorFormat::Gray8 && to == ColorFormat::RGB8) {
    for (size_t i = 0; i < n; ++i, d += 3) {
      d[0] = d[1] = d[2] = s[i];
    }
  } else {
    throw std::invalid_argument("convert: unsupported format pair");
  }
  return out;
}

}  // namespace prmd
