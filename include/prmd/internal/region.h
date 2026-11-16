#pragma once
// A rectangle of int32 samples, addressed in the coordinates of the plane it
// belongs to.

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "prmd/image.h"

namespace prmd::internal {

// std::allocator that can skip value-initialization. A vector created with
// SampleAllocator<T>(false) leaves new elements uninitialized (resize, the
// size constructor), so a large plane that a pass is about to overwrite
// entirely is not zeroed first, and its pages are first touched by the
// threads that write them. Default-constructed (and every copy-constructed)
// vector zeroes new elements, exactly like std::vector<T>. All instances
// compare equal: memory moves freely between vectors.
template <class T>
class SampleAllocator : public std::allocator<T> {
 public:
  template <class U>
  struct rebind {
    using other = SampleAllocator<U>;
  };
  using propagate_on_container_move_assignment = std::true_type;
  using is_always_equal = std::true_type;

  SampleAllocator() noexcept = default;
  explicit SampleAllocator(bool zero) noexcept : zero_(zero) {}
  template <class U>
  SampleAllocator(const SampleAllocator<U>& o) noexcept : zero_(o.zero()) {}

  bool zero() const noexcept { return zero_; }

  // New elements: value- (zero) or default- (no) initialization.
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    if (zero_) {
      ::new (static_cast<void*>(p)) U();
    } else {
      ::new (static_cast<void*>(p)) U;
    }
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) noexcept(
      std::is_nothrow_constructible_v<U, Args...>) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }

  // Copies zero-initialize again.
  SampleAllocator select_on_container_copy_construction() const noexcept {
    return SampleAllocator();
  }

 private:
  bool zero_ = true;
};

template <class T, class U>
bool operator==(const SampleAllocator<T>&, const SampleAllocator<U>&) noexcept {
  return true;
}
template <class T, class U>
bool operator!=(const SampleAllocator<T>&, const SampleAllocator<U>&) noexcept {
  return false;
}

// Integer samples of a plane or code-block. Behaves as std::vector<int32_t>
// (zeroed elements) unless created with uninitialized_samples().
using Samples = std::vector<int32_t, SampleAllocator<int32_t>>;

// n samples with unspecified values: every one must be written before it is
// read.
inline Samples uninitialized_samples(size_t n) {
  return Samples(n, SampleAllocator<int32_t>(false));
}

// Tag for Region's uninitialized constructor.
struct NoInit {};
inline constexpr NoInit kNoInit{};

struct Region {
  Rect rect;  // position inside the owning plane
  Samples v;  // rect.w * rect.h, row-major

  Region() = default;
  // Zeroed samples.
  explicit Region(const Rect& r) : rect(r), v(area(r), 0) {}
  // Unspecified samples, for a plane the caller writes entirely before
  // reading any of it (encoder passes: no serial zero-fill, first touch by
  // the writing threads).
  Region(const Rect& r, NoInit) : rect(r), v(uninitialized_samples(area(r))) {}

  // Absolute plane coordinates.
  int32_t& at(int x, int y) { return v[offset(x, y)]; }
  int32_t at(int x, int y) const { return v[offset(x, y)]; }

  // Start of row y (absolute); element i is column rect.x + i.
  int32_t* row(int y) { return v.data() + offset(rect.x, y); }
  const int32_t* row(int y) const { return v.data() + offset(rect.x, y); }

  bool covers(const Rect& r) const {
    return r.x >= rect.x && r.y >= rect.y && r.right() <= rect.right() &&
           r.bottom() <= rect.bottom();
  }

 private:
  static size_t area(const Rect& r) {
    return size_t(r.w > 0 ? r.w : 0) * size_t(r.h > 0 ? r.h : 0);
  }

  size_t offset(int x, int y) const {
    return size_t(y - rect.y) * size_t(rect.w) + size_t(x - rect.x);
  }
};

// Copies the overlap of `src` into `dst` (both in the same plane coordinates).
void copy_overlap(const Region& src, Region& dst);

}  // namespace prmd::internal
