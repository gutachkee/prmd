#pragma once
// Opt-in wall-clock profiler for the encode and decode pipelines.
//
// The library marks each pipeline step with PRMD_TIMED("step"). Unless the
// macro PRMD_PROFILE is defined when prmd_core is compiled (CMake:
// -DPRMD_PROFILE=ON, which defines it for prmd_core and everything linking
// it), PRMD_TIMED expands to nothing: no clock reads, no bookkeeping, no code.
//
// When enabled, every PRMD_TIMED scope reads std::chrono::steady_clock on
// entry and exit and adds the difference to a node of a call tree. A scope
// opened while another is open becomes its child, so a parent's time includes
// its children. Nodes are merged by (parent, name): a step that runs once per
// block accumulates all its calls into one node with a call count.
//
// Threads. The tree is per thread (thread_local): reset() and snapshot() see
// the calling thread's tree. A multithreaded encode (EncodeOptions::threads
// != 1) runs parts of a step on pool worker threads; the pool merges what
// the workers recorded into the calling thread's tree, under the scope that
// was open on the calling thread when the work was handed out, at the end of
// every parallel section. What the numbers then mean:
//   * A scope opened on the calling thread around parallel work (e.g.
//     "encode_blocks", "forward", "quantize") is WALL time: its ms is the
//     elapsed time of the step, and worker_ms is 0.
//   * A scope opened inside the parallel work (e.g. "make_block",
//     "entropy_encode", "zstd_compress", once per block on whichever thread
//     ran the block) is SUMMED THREAD time: its ms adds up the time of every
//     thread that ran it, roughly CPU time, so it can exceed its parent's
//     wall time (by up to the thread count). worker_ms / worker_calls are the
//     part contributed by worker threads; ms - worker_ms is the calling
//     thread's own share.
// With threads = 1 nothing runs on another thread, every number is wall time
// on the calling thread and worker_ms is 0, as before threading existed.
// Comparing a child's ms with its parent's at N threads gives the parallel
// efficiency of that step: ms(child) / (N * ms(parent)).
//
// Usage (see tools/prmd_pipeline.cpp):
//   prmd::profile::reset();
//   ... encode / decode ...
//   for (const auto& e : prmd::profile::snapshot()) { e.path, e.ms, e.calls }

#include <cstdint>
#include <string>
#include <vector>

namespace prmd::profile {

#ifdef PRMD_PROFILE
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

struct Entry {
  std::string path;  // names from the root, joined by '/': "encode/blocks/zstd"
  std::string name;  // last component of path
  int depth = 0;     // 0 for top-level scopes
  uint64_t calls = 0;  // all threads
  // Total time over all calls, children included: wall time for scopes on
  // the calling thread, summed over threads for scopes that also ran on pool
  // workers (see "Threads" above).
  double ms = 0;
  uint64_t worker_calls = 0;  // part of calls made on pool worker threads
  double worker_ms = 0;       // part of ms measured on pool worker threads
};

// Clears this thread's tree. Must not be called while a scope is open.
// No-op when profiling is compiled out.
void reset();

// This thread's tree in depth-first order, children in first-seen order.
// Empty when profiling is compiled out.
std::vector<Entry> snapshot();

#ifdef PRMD_PROFILE
// RAII timer behind PRMD_TIMED. `name` must be a string literal (or outlive
// the profile): nodes keep the pointer.
class Scope {
 public:
  explicit Scope(const char* name);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  void* node_;
  int64_t start_ns_;
};
#endif

}  // namespace prmd::profile

#define PRMD_PROFILE_CAT2(a, b) a##b
#define PRMD_PROFILE_CAT(a, b) PRMD_PROFILE_CAT2(a, b)

#ifdef PRMD_PROFILE
// Times the rest of the enclosing block as step `name`.
#define PRMD_TIMED(name) \
  ::prmd::profile::Scope PRMD_PROFILE_CAT(prmd_timed_, __LINE__)(name)
#else
#define PRMD_TIMED(name) static_cast<void>(0)
#endif
