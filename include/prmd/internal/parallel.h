#pragma once
// Fixed-size thread pool for the encoder (design.md §1.4, "Threads").
//
// One Pool is created per encode call and destroyed with it; there is no
// global pool, so concurrent encodes from different user threads stay
// independent. Work is flat: Pool::run hands out chunks of an index range
// [0, n) in increasing order from one atomic counter (dynamic
// self-scheduling), the calling thread works too, and run returns when every
// chunk is done. Results must not depend on which thread ran which chunk;
// callers write each index's result to its own slot, so the output is the
// same for every thread count.
//
// Determinism of failures: if chunks throw, the exception of the LOWEST
// failing chunk is rethrown on the calling thread once all workers are idle.
// Chunks are claimed in increasing order, so every chunk below a failing one
// has already been claimed and runs to completion, and chunk bodies process
// their indices in increasing order: the exception rethrown is exactly the
// one a serial loop over [0, n) would have thrown first, whatever the thread
// count. Unclaimed chunks after a failure are skipped.
//
// Profiling (PRMD_PROFILE): PRMD_TIMED scopes opened on workers are merged
// into the calling thread's tree under the scope that was open around run(),
// so the per-step numbers include worker time (see prmd/profile.h).

#include <cstddef>
#include <functional>
#include <memory>

namespace prmd::internal {

constexpr int kMaxThreads = 256;

// Thread count for EncodeOptions::threads = `requested`: 0 means
// `hardware` (std::thread::hardware_concurrency(); 1 if that is 0, i.e.
// unknown), positive values are used as given; the result is clamped to
// [1, kMaxThreads]. Throws std::invalid_argument for requested < 0.
int resolve_threads(int requested, unsigned hardware);
int resolve_threads(int requested);  // hardware = hardware_concurrency()

class Pool {
 public:
  using Fn = std::function<void(size_t begin, size_t end)>;

  // A pool of `threads` threads in total: the caller plus threads - 1
  // workers, started here and joined by the destructor. threads <= 1 starts
  // none. If the system refuses to start a thread, the pool keeps the ones it
  // has (size() tells how many); the caller thread alone always works.
  explicit Pool(int threads);
  ~Pool();
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  // Threads that run work, the caller included (>= 1).
  int size() const;

  // Calls fn(begin, end) for consecutive chunks of [0, n), each `grain`
  // indices long (the last one shorter), and blocks until all are done.
  // Runs fn(0, n) directly on the calling thread, with no synchronization,
  // when n <= grain, when the pool has no workers, or when called from
  // inside another run() (nesting is not parallel). grain 0 counts as 1.
  // Not reentrant across threads: one run() at a time per pool.
  void run(size_t n, size_t grain, const Fn& fn);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Target number of samples per chunk for row and sample loops: large enough
// to amortize the claim (one atomic add) and small enough to balance load and
// to split the levels of small images too.
constexpr size_t kChunkSamples = size_t(1) << 14;

// pool->run(n, grain, fn), or fn(0, n) when pool is null (the serial
// encoder: no pool exists).
void parallel_for(Pool* pool, size_t n, size_t grain, const Pool::Fn& fn);

// fn(y0, y1) over row stripes of [0, h) for rows `width` samples wide, about
// kChunkSamples samples per stripe; fn(0, h) when pool is null.
void parallel_rows(Pool* pool,
                   int h,
                   int width,
                   const std::function<void(int y0, int y1)>& fn);

// fn(begin, end) over [0, n) in chunks of kChunkSamples; fn(0, n) when pool
// is null.
void parallel_samples(Pool* pool, size_t n, const Pool::Fn& fn);

}  // namespace prmd::internal
