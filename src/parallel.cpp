// Fixed-size thread pool (include/prmd/internal/parallel.h).

#include "prmd/internal/parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "prmd/profile.h"
#ifdef PRMD_PROFILE
#include "prmd/internal/profile_threads.h"
#endif

namespace prmd::internal {

namespace {

// Set while this thread executes chunks of some run(): a nested run() then
// executes inline instead of re-entering a pool.
thread_local bool tl_in_run = false;

struct InRun {
  InRun() { tl_in_run = true; }
  ~InRun() { tl_in_run = false; }
  InRun(const InRun&) = delete;
  InRun& operator=(const InRun&) = delete;
};

std::atomic<int> g_worker_start_limit{-1};
std::atomic<test_hooks::WorkerStartFailure> g_worker_start_failure{
    test_hooks::WorkerStartFailure::Refused};
std::atomic<test_hooks::RowStripeObserver> g_row_observer{nullptr};

}  // namespace

namespace test_hooks {
void set_worker_start_limit(int k, WorkerStartFailure failure) {
  g_worker_start_failure.store(failure);
  g_worker_start_limit.store(k);
}
void set_row_stripe_observer(RowStripeObserver observer) {
  g_row_observer.store(observer);
}
}  // namespace test_hooks

void validate_threads(int requested) {
  if (requested < 0) {
    throw std::invalid_argument("threads must be >= 0 (0 = automatic)");
  }
}

int resolve_threads(int requested, unsigned hardware) {
  validate_threads(requested);
  long n = requested;
  if (n == 0) {
    n = hardware == 0 ? 1 : long(std::min<unsigned>(hardware, kMaxThreads));
  }
  return int(std::clamp<long>(n, 1, kMaxThreads));
}

int resolve_threads(int requested) {
  return resolve_threads(requested, std::thread::hardware_concurrency());
}

int auto_threads(uint64_t work, unsigned hardware) {
  const uint64_t cap = uint64_t(resolve_threads(0, hardware));
  const uint64_t q = work / kAutoWorkGrain;
  // floor(sqrt(q)), exact for every uint64_t.
  uint64_t r = uint64_t(std::sqrt(double(q)));
  while (r > 0 && r * r > q) {
    --r;
  }
  while ((r + 1) * (r + 1) <= q && r + 1 <= cap) {
    ++r;
  }
  return int(std::clamp<uint64_t>(r, 1, cap));
}

int choose_threads(int requested, uint64_t work, unsigned hardware) {
  validate_threads(requested);
  return requested == 0 ? auto_threads(work, hardware)
                        : resolve_threads(requested, hardware);
}

int choose_threads(int requested, uint64_t work) {
  return choose_threads(requested, work, std::thread::hardware_concurrency());
}

// Synchronization. The job (fn, n, grain, next, cancel) is written by the
// caller and published with one atomic word holding (generation, seats):
// generation counts parallel jobs, seats is how many more workers may join
// the current one. A worker joins by taking a seat with a CAS on that word,
// which also tells it which job it joined; `active` counts workers that
// joined (or are about to) and have not finished. The caller closes the job
// (seats = 0) once it finds no chunk left, then waits for active == 0: it
// does not wait for workers that arrive after that, only for those that
// took a seat (one preempted after taking it can still delay the caller).
//
// Idle workers first spin for kSpin, checking the word, then sleep on
// `wake`: stages follow each other within microseconds, and a spinning
// worker joins the next one without a futex wake-up (tens of microseconds
// here). The caller spins the same way before sleeping on `done`.
namespace {

constexpr auto kSpin = std::chrono::microseconds(100);
constexpr uint64_t kSeatBits = 16;
constexpr uint64_t kSeatMask = (uint64_t(1) << kSeatBits) - 1;

uint64_t pack(uint64_t generation, int seats) {
  return (generation << kSeatBits) | uint64_t(seats);
}
uint64_t generation_of(uint64_t word) {
  return word >> kSeatBits;
}
int seats_of(uint64_t word) {
  return int(word & kSeatMask);
}

inline void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#endif
}

// Spins until pred() or kSpin has passed; returns pred().
template <class Pred>
bool spin_until(Pred pred) {
  const auto deadline = std::chrono::steady_clock::now() + kSpin;
  for (unsigned i = 1;; ++i) {
    if (pred()) {
      return true;
    }
    if (i % 64 == 0 && std::chrono::steady_clock::now() >= deadline) {
      return pred();
    }
    cpu_relax();
  }
}

}  // namespace

struct Pool::Impl {
  std::vector<std::thread> threads;

  std::mutex m;
  std::condition_variable wake;  // sleeping workers: a new job (or stop)
  std::condition_variable done;  // sleeping caller: active reached 0

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> word{0};    // pack(generation, seats)
  std::atomic<int> active{0};       // joined workers not finished yet
  std::atomic<int> sleepers{0};     // workers blocked on `wake`

  // The current job: written by the caller before it publishes `word`.
  const Fn* fn = nullptr;
  size_t n = 0;
  size_t grain = 1;
  // Lock-free chunk claiming.
  std::atomic<size_t> next{0};
  std::atomic<bool> cancel{false};

  // Guarded by m.
  size_t fail_chunk = SIZE_MAX;  // begin index of the lowest failing chunk
  std::exception_ptr fail;
#ifdef PRMD_PROFILE
  std::vector<profile::detail::SubtreePtr> trees;  // one slot per worker
#endif

  // Claims and runs chunks until none are left (or one failed).
  void work() {
    const InRun in_run;
    for (;;) {
      if (cancel.load(std::memory_order_relaxed)) {
        return;
      }
      const size_t b = next.fetch_add(grain, std::memory_order_relaxed);
      if (b >= n) {
        return;
      }
      const size_t e = std::min(n, b + grain);
#ifdef PRMD_PROFILE
      profile::detail::begin_chunk(b);
#endif
      try {
        (*fn)(b, e);
      } catch (...) {
        const std::lock_guard<std::mutex> lock(m);
        if (b < fail_chunk) {
          fail_chunk = b;
          fail = std::current_exception();
        }
        // Every chunk not claimed yet starts above b.
        cancel.store(true, std::memory_order_relaxed);
      }
#ifdef PRMD_PROFILE
      profile::detail::end_chunk();
#endif
    }
  }

  // Waits (spin, then sleep) for a job newer than `seen`; returns its word,
  // or 0 to stop.
  uint64_t await_job(uint64_t seen) {
    uint64_t w = 0;
    const auto fresh = [&] {
      if (stop.load(std::memory_order_acquire)) {
        return true;
      }
      w = word.load(std::memory_order_acquire);
      return generation_of(w) != seen;
    };
    if (!spin_until(fresh)) {
      std::unique_lock<std::mutex> lock(m);
      sleepers.fetch_add(1, std::memory_order_seq_cst);
      wake.wait(lock, [&] {
        // seq_cst: pairs with run()'s store of `word` and load of
        // `sleepers` (one of the two sees the other).
        if (stop.load(std::memory_order_seq_cst)) {
          return true;
        }
        w = word.load(std::memory_order_seq_cst);
        return generation_of(w) != seen;
      });
      sleepers.fetch_sub(1, std::memory_order_relaxed);
    }
    return stop.load(std::memory_order_acquire) ? 0 : w;
  }

  // Takes a seat of the job of `w`; false if it is full or closed.
  bool join(uint64_t w) {
    if (seats_of(w) == 0) {
      return false;
    }
    active.fetch_add(1, std::memory_order_acq_rel);  // before the seat
    while (seats_of(w) > 0) {
      if (word.compare_exchange_weak(w, w - 1, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
        return true;
      }
    }
    leave();
    return false;
  }

  void leave() {
    if (active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // The caller may be asleep on `done`.
      { const std::lock_guard<std::mutex> lock(m); }
      done.notify_one();
    }
  }

  void worker_main(int index) {
    (void)index;
    uint64_t seen = 0;
    for (;;) {
      const uint64_t w = await_job(seen);
      if (w == 0) {
        return;
      }
      seen = generation_of(w);
      uint64_t joined = w;
      if (!join(joined)) {
        continue;
      }
      // The CAS may have taken a seat of a newer job than `w`'s: the one in
      // the word now, as no job starts before this one finishes.
      seen = generation_of(word.load(std::memory_order_acquire));
#ifdef PRMD_PROFILE
      profile::detail::begin_worker();
#endif
      work();
#ifdef PRMD_PROFILE
      profile::detail::SubtreePtr tree = profile::detail::end_worker();
      if (tree) {
        const std::lock_guard<std::mutex> lock(m);
        trees[size_t(index)] = std::move(tree);
      }
#endif
      leave();
    }
  }
};

Pool::Pool(int threads) : impl_(std::make_unique<Impl>()) {
  const int workers = std::clamp(threads, 1, kMaxThreads) - 1;
#ifdef PRMD_PROFILE
  impl_->trees.resize(size_t(workers));
#endif
  impl_->threads.reserve(size_t(workers));
  const int limit = g_worker_start_limit.load();
  const auto failure = g_worker_start_failure.load();
  for (int i = 0; i < workers; ++i) {
    try {
      if (limit >= 0 && i >= limit) {
        if (failure == test_hooks::WorkerStartFailure::OutOfMemory) {
          throw std::bad_alloc();
        }
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again),
            "worker start limit (test hook)");
      }
      impl_->threads.emplace_back([impl = impl_.get(), i] {
        impl->worker_main(i);
      });
    } catch (const std::system_error&) {
      break;  // run with the threads we have
    } catch (...) {
      // Anything else (std::bad_alloc) fails the pool: the workers already
      // started must be joined before their Impl goes away.
      stop_and_join();
      throw;
    }
  }
}

void Pool::stop_and_join() {
  impl_->stop.store(true, std::memory_order_seq_cst);
  { const std::lock_guard<std::mutex> lock(impl_->m); }
  impl_->wake.notify_all();
  for (std::thread& t : impl_->threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

Pool::~Pool() {
  stop_and_join();
}

int Pool::size() const {
  return 1 + int(impl_->threads.size());
}

void Pool::run(size_t n, size_t grain, const Fn& fn) {
  if (n == 0) {
    return;
  }
  grain = std::max<size_t>(grain, 1);
  Impl& s = *impl_;
  if (s.threads.empty() || n <= grain || tl_in_run) {
    fn(0, n);
    return;
  }

  // The caller takes the first chunk itself: at most one worker per further
  // chunk.
  const size_t chunks = (n - 1) / grain + 1;
  const int helpers = int(std::min<size_t>(s.threads.size(), chunks - 1));
#ifdef PRMD_PROFILE
  void* const scope = profile::detail::current_scope();
#endif
  // No worker is active: the previous job ended with active == 0, and its
  // seats were closed.
  s.fn = &fn;
  s.n = n;
  s.grain = grain;
  s.next.store(0, std::memory_order_relaxed);
  s.cancel.store(false, std::memory_order_relaxed);
  {
    const std::lock_guard<std::mutex> lock(s.m);
    s.fail_chunk = SIZE_MAX;
    s.fail = nullptr;
  }
  const uint64_t generation =
      generation_of(s.word.load(std::memory_order_relaxed)) + 1;
  s.word.store(pack(generation, helpers), std::memory_order_seq_cst);
  const int asleep = s.sleepers.load(std::memory_order_seq_cst);
  if (asleep > 0) {
    { const std::lock_guard<std::mutex> lock(s.m); }
    for (int i = 0; i < std::min(asleep, helpers); ++i) {
      s.wake.notify_one();
    }
  }

  s.work();  // the caller takes chunks too

  // Every chunk is claimed (or the job was cancelled): a worker that has not
  // joined yet would find nothing to do, so close the seats and wait only
  // for those that took one.
  // An RMW, so that it reads every seat taken before it: the takers'
  // increments of `active` are then visible below.
  s.word.exchange(pack(generation, 0), std::memory_order_seq_cst);
  const auto idle = [&] {
    return s.active.load(std::memory_order_acquire) == 0;
  };
  if (!spin_until(idle)) {
    std::unique_lock<std::mutex> lock(s.m);
    s.done.wait(lock, idle);
  }
  std::exception_ptr fail;
  {
    const std::lock_guard<std::mutex> lock(s.m);
    s.fn = nullptr;
    fail = std::move(s.fail);
    s.fail = nullptr;
  }
#ifdef PRMD_PROFILE
  // Workers are idle now; their trees are only touched here.
  for (auto& t : s.trees) {
    if (t) {
      profile::detail::merge_worker(scope, *t);
      t.reset();
    }
  }
  profile::detail::canonicalize(scope);
#endif
  if (fail) {
    std::rethrow_exception(fail);
  }
}

void parallel_for(Pool* pool, size_t n, size_t grain, const Pool::Fn& fn) {
  if (!pool) {
    if (n > 0) {
      fn(0, n);
    }
    return;
  }
  pool->run(n, grain, fn);
}

void parallel_rows(Pool* pool,
                   int h,
                   size_t row_samples,
                   const std::function<void(int, int)>& fn) {
  if (h <= 0) {
    return;
  }
  const test_hooks::RowStripeObserver observe = g_row_observer.load();
  if (!pool) {
    if (observe) {
      observe(0, h);
    }
    fn(0, h);
    return;
  }
  if (size_t(h) * row_samples <= kMinParallelSamples) {
    if (observe) {
      observe(0, h);
    }
    fn(0, h);  // too little work to hand out
    return;
  }
  const size_t rows =
      std::max<size_t>(1, kChunkSamples / std::max<size_t>(row_samples, 1));
  pool->run(size_t(h), rows, [&](size_t b, size_t e) {
    if (observe) {
      observe(int(b), int(e));
    }
    fn(int(b), int(e));
  });
}

void parallel_samples(Pool* pool, size_t n, const Pool::Fn& fn) {
  if (n <= kMinParallelSamples) {
    if (n > 0) {
      fn(0, n);  // too little work to hand out
    }
    return;
  }
  parallel_for(pool, n, kChunkSamples, fn);
}

}  // namespace prmd::internal
