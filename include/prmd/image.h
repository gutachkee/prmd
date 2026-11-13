#pragma once
// Basic types: Rect, ColorFormat, Image.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace prmd {

// Pixel rectangle: top-left corner (x, y), size w × h. Covers columns x .. x+w−1
// and rows y .. y+h−1.
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  int right() const { return x + w; }   // one past the last column (exclusive end)
  int bottom() const { return y + h; }  // one past the last row (exclusive end)
  bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
  bool operator!=(const Rect& o) const { return !(*this == o); }
};

// Returns an empty Rect ({0,0,0,0}) if a and b don't overlap.
Rect intersect(const Rect& a, const Rect& b);

// Pixel format identifier. The numeric values are stable on-disk ids, NOT channel counts:
// several formats may share a channel count. New formats get the next free id.
enum class ColorFormat : uint8_t {
  Gray8 = 0,  // 1 channel: luma
  RGB8 = 1,   // 3 channels: R, G, B (in that order)
};

struct ColorFormatInfo {
  ColorFormat format;
  const char* name;  // CLI spelling: "gray", "rgb"
  int channels;
};

// Throws std::invalid_argument for unknown ids.
const ColorFormatInfo& info(ColorFormat f);
inline int channels(ColorFormat f) { return info(f).channels; }
// True if `id` names a known ColorFormat.
bool is_valid_color_format(uint8_t id);

// 8-bit image, row-major, channels interleaved (RGBRGB…), tightly packed:
// stride = width * channels.
struct Image {
  int width = 0, height = 0;
  ColorFormat format = ColorFormat::Gray8;
  std::vector<uint8_t> pixels;

  Image() = default;
  Image(int w, int h, ColorFormat f = ColorFormat::Gray8);  // zero-filled

  int channels() const { return prmd::channels(format); }
  size_t stride() const { return size_t(width) * size_t(channels()); }
  uint8_t* row(int y) { return pixels.data() + size_t(y) * stride(); }
  const uint8_t* row(int y) const { return pixels.data() + size_t(y) * stride(); }
  // Sample at column x, row y, channel `channel`. Gray8 has only 0; RGB8 has 0 = R, 1 = G, 2 = B.
  uint8_t at(int x, int y, int channel = 0) const {
    return row(y)[size_t(x) * size_t(channels()) + size_t(channel)];
  }
  Rect bounds() const { return Rect{0, 0, width, height}; }
  // Plain copy of r; throws std::out_of_range if r is empty or not inside the image.
  Image crop(const Rect& r) const;
};

// RGB→Gray uses BT.601 integer (77R + 150G + 29B + 128) >> 8.
// Gray→RGB replicates the value into all three channels. Same format → copy.
Image convert(const Image& src, ColorFormat to);

}  // namespace prmd
