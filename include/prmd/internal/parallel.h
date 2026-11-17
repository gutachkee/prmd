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
// Wake-ups: run() wakes only as many workers as the job has chunks beyond
// the caller's first, and a worker that wakes after every chunk has been
// claimed does not join, so run() does not wait for workers that arrive
// after the seats are closed (thread wake-up latency is tens of
// microseconds). It does wait for every worker that took a seat: one that
// is preempted after joining, even if it then finds no chunk left, can
// delay it.
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
// in the order a serial loop would have created them (see prmd/profile.h).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace prmd::internal {

constexpr int kMaxThreads = 256;

// Throws std::invalid_argument for EncodeOptions::threads < 0 (the one check
// shared by the .prmd and J2K encoders and the tools).
void validate_threads(int requested);

// Upper bound on the threads of an encode for EncodeOptions::threads =
// `requested`: 0 (automatic) means `hardware`
// (std::thread::hardware_concurrency(); 1 if that is 0, i.e. unknown),
// positive values are used as given; the result is clamped to
// [1, kMaxThreads]. Throws std::invalid_argument for requested < 0.
int resolve_threads(int requested, unsigned hardware);
int resolve_threads(int requested);  // hardware = hardware_concurrency()

// Automatic thread count (EncodeOptions::threads = 0) for an encode of
// `work` work units: n threads once the work reaches n² × kAutoWorkGrain,
// i.e. floor(sqrt(work / kAutoWorkGrain)), at least 1, at most
// resolve_threads(0, hardware). A pure function, so the choice is
// deterministic for a given image, options and host.
//
// A work unit is the encoder's serial time for one level-0 sample of a gray
// haar + zstd-1 encode (about 4.3 ns on the calibration host); slower modes
// and coders weigh their samples more (prmd_work_units() in prmd_codec.h,
// the J2K backend in j2k_codec.cpp). Calibrated on a 14-core host
// (docs/benchmarks.md, "Automatic thread count"): a thread costs about
// 0.1-0.2 ms to start, join and feed, and the stages that bound memory
// bandwidth gain less from each added thread, so the best count grows with
// the square root of the work rather than linearly. Below 4 × kAutoWorkGrain
// (about 0.56 ms of serial work) one thread is used and no thread started.
constexpr uint64_t kAutoWorkGrain = uint64_t(1) << 15;  // 32 Ki units
int auto_threads(uint64_t work, unsigned hardware);

// The thread count of an encode: requested == 0 → auto_threads(work,
// hardware); requested > 0 → resolve_threads(requested, hardware) (an upper
// bound honoured as given, clamped to kMaxThreads). Callers cap it further
// by their unit of work (code-blocks). Throws for requested < 0.
int choose_threads(int requested, uint64_t work, unsigned hardware);
int choose_threads(int requested, uint64_t work);  // hardware_concurrency()

class Pool {
 public:
  using Fn = std::function<void(size_t begin, size_t end)>;

  // A pool of `threads` threads in total: the caller plus threads - 1
  // workers, started here and joined by the destructor. threads <= 1 starts
  // none. If the system refuses to start a thread (std::system_error), the
  // pool keeps the ones it has (size() tells how many); the caller thread
  // alone always works. Any other exception while starting them
  // (std::bad_alloc) is rethrown once the workers started are joined.
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
  void stop_and_join();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Target number of samples per chunk for row and sample loops: large enough
// to amortize the claim (one atomic add) and small enough to balance load and
// to split the levels of small images too.
constexpr size_t kChunkSamples = size_t(1) << 14;

// Row and sample loops of at most this many samples run inline on the
// calling thread even with a pool: handing out two chunks costs about as
// much as it saves (measured, docs/benchmarks.md).
constexpr size_t kMinParallelSamples = 2 * kChunkSamples;

// pool->run(n, grain, fn), or fn(0, n) when pool is null (the serial
// encoder: no pool exists).
void parallel_for(Pool* pool, size_t n, size_t grain, const Pool::Fn& fn);

// fn(y0, y1) over row stripes of [0, h) for rows that cost about
// `row_samples` samples each, about kChunkSamples samples per stripe;
// fn(0, h) when pool is null or h * row_samples <= kMinParallelSamples.
void parallel_rows(Pool* pool,
                   int h,
                   size_t row_samples,
                   const std::function<void(int y0, int y1)>& fn);

// fn(begin, end) over [0, n) in chunks of kChunkSamples; fn(0, n) when pool
// is null or n <= kMinParallelSamples.
void parallel_samples(Pool* pool, size_t n, const Pool::Fn& fn);

// Test hooks (tests/test_parallel.cpp, tests/test_threads.cpp). Process-wide;
// off unless a test sets them, and not for use outside tests.
namespace test_hooks {
// How a worker start beyond the limit fails: Refused throws
// std::system_error, as std::thread does when the system refuses a thread
// (the pool keeps the workers it has); OutOfMemory throws std::bad_alloc
// (the Pool constructor joins the workers it started and rethrows).
enum class WorkerStartFailure { Refused, OutOfMemory };
// Pools start at most `k` worker threads from now on: starting the next one
// fails as `failure` says, exercising the fewer-threads fallback (or the
// failed constructor). k < 0 turns the limit off (the default).
void set_worker_start_limit(
    int k, WorkerStartFailure failure = WorkerStartFailure::Refused);
// Called with every row stripe parallel_rows hands out (on the thread that
// runs it, before fn); nullptr turns it off (the default).
using RowStripeObserver = void (*)(int y0, int y1);
void set_row_stripe_observer(RowStripeObserver observer);
}  // namespace test_hooks

}  // namespace prmd::internal
