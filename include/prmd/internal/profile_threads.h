#pragma once
// Profiler hooks for the thread pool (src/parallel.cpp); only compiled with
// PRMD_PROFILE. A worker records into its own thread_local tree during one
// Pool::run, detaches it, and the calling thread merges it under the scope
// that was open around run() once all workers are idle (no tree is ever
// touched by two threads at once).

#ifdef PRMD_PROFILE

#include <cstddef>
#include <memory>

namespace prmd::profile::detail {

struct Subtree;  // a detached per-thread tree
struct SubtreeDeleter {
  void operator()(Subtree* t) const;
};
using SubtreePtr = std::unique_ptr<Subtree, SubtreeDeleter>;

// The calling thread's innermost open PRMD_TIMED scope (its root when none
// is open), as an opaque handle for merge_worker on the same thread.
void* current_scope();

// Starts an empty tree on this (worker) thread. No scope may be open.
void begin_worker();

// Detaches this thread's tree (null if nothing was recorded) and leaves an
// empty one.
SubtreePtr end_worker();

// Adds the nodes of `t` under `scope` (from current_scope() on this thread),
// by name, as worker time. Call only on the thread that owns `scope`.
void merge_worker(void* scope, Subtree& t);

// Chunk [b, ...) of a Pool::run starts / ends on this thread. Nodes first
// created inside a chunk remember (b, creation order within the chunk), so
// that canonicalize() can restore the order a serial loop over [0, n) would
// have created them in, whichever thread ran which chunk.
void begin_chunk(size_t b);
void end_chunk();

// After every worker tree of a run has been merged under `scope`: orders the
// children of every node below `scope` by first appearance in a serial run
// (nodes that existed before the run first, in their order; then by the
// lowest chunk that created them and their order within it) and forgets the
// chunk keys. snapshot()'s entry order then does not depend on scheduling.
void canonicalize(void* scope);

}  // namespace prmd::profile::detail

#endif
