#pragma once
// Wall-clock helpers for encode reports and benchmarks.

#include <chrono>

namespace prmd::internal {

using Clock = std::chrono::steady_clock;

// Milliseconds elapsed since `start`.
inline double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

}  // namespace prmd::internal
