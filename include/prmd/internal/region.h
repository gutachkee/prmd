#pragma once
// A rectangle of int32 samples, addressed in the coordinates of the plane it belongs to.

#include <cstdint>
#include <vector>

#include "prmd/image.h"

namespace prmd::internal {

struct Region {
  Rect rect;                // position inside the owning plane
  std::vector<int32_t> v;   // rect.w * rect.h, row-major

  Region() = default;
  explicit Region(const Rect& r) : rect(r), v(size_t(r.w > 0 ? r.w : 0) * size_t(r.h > 0 ? r.h : 0), 0) {}

  // Absolute plane coordinates.
  int32_t& at(int x, int y) { return v[size_t(y - rect.y) * size_t(rect.w) + size_t(x - rect.x)]; }
  int32_t at(int x, int y) const { return v[size_t(y - rect.y) * size_t(rect.w) + size_t(x - rect.x)]; }
  // Start of row y (absolute); element i is column rect.x + i.
  int32_t* row(int y) { return v.data() + size_t(y - rect.y) * size_t(rect.w); }
  const int32_t* row(int y) const { return v.data() + size_t(y - rect.y) * size_t(rect.w); }
  bool covers(const Rect& r) const {
    return r.x >= rect.x && r.y >= rect.y && r.right() <= rect.right() && r.bottom() <= rect.bottom();
  }
};

// Copies the overlap of `src` into `dst` (both in the same plane coordinates).
void copy_overlap(const Region& src, Region& dst);

}  // namespace prmd::internal
