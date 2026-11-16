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
// block accumulates all its calls into one node with a call count. The tree
// is per thread (thread_local); prmd decodes and encodes on one thread.
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
  uint64_t calls = 0;
  double ms = 0;     // total wall time over all calls, children included
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
