// Fixed-size thread pool (include/prmd/internal/parallel.h).

#include "prmd/internal/parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
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

}  // namespace

int resolve_threads(int requested, unsigned hardware) {
  if (requested < 0) {
    throw std::invalid_argument(
        "threads must be >= 0 (0 = one per hardware thread)");
  }
  long n = requested;
  if (n == 0) {
    n = hardware == 0 ? 1 : long(std::min<unsigned>(hardware, kMaxThreads));
  }
  return int(std::clamp<long>(n, 1, kMaxThreads));
}

int resolve_threads(int requested) {
  return resolve_threads(requested, std::thread::hardware_concurrency());
}

struct Pool::Impl {
  std::vector<std::thread> threads;

  std::mutex m;
  std::condition_variable wake;  // workers: a new job (or stop)
  std::condition_variable done;  // caller: the last participant finished

  // Guarded by m.
  bool stop = false;
  uint64_t generation = 0;  // bumped once per parallel job
  int participants = 0;     // workers 0 .. participants-1 work on the job
  int pending = 0;          // participants not finished yet
  const Fn* fn = nullptr;
  size_t n = 0;
  size_t grain = 1;
  size_t fail_chunk = SIZE_MAX;  // begin index of the lowest failing chunk
  std::exception_ptr fail;
#ifdef PRMD_PROFILE
  std::vector<profile::detail::SubtreePtr> trees;  // one slot per worker
#endif

  // Lock-free chunk claiming; published to workers under m with the job.
  std::atomic<size_t> next{0};
  std::atomic<bool> cancel{false};

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
    }
  }

  void worker_main(int index) {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(m);
        wake.wait(lock, [&] { return stop || generation != seen; });
        if (stop) {
          return;
        }
        seen = generation;
        if (index >= participants) {
          continue;  // job too small to need this worker
        }
      }
#ifdef PRMD_PROFILE
      profile::detail::begin_worker();
#endif
      work();
#ifdef PRMD_PROFILE
      profile::detail::SubtreePtr tree = profile::detail::end_worker();
#endif
      const std::lock_guard<std::mutex> lock(m);
#ifdef PRMD_PROFILE
      trees[size_t(index)] = std::move(tree);
#endif
      if (--pending == 0) {
        done.notify_one();
      }
    }
  }
};

Pool::Pool(int threads) : impl_(std::make_unique<Impl>()) {
  const int workers = std::clamp(threads, 1, kMaxThreads) - 1;
#ifdef PRMD_PROFILE
  impl_->trees.resize(size_t(workers));
#endif
  impl_->threads.reserve(size_t(workers));
  for (int i = 0; i < workers; ++i) {
    try {
      impl_->threads.emplace_back([impl = impl_.get(), i] {
        impl->worker_main(i);
      });
    } catch (const std::system_error&) {
      break;  // run with the threads we have
    }
  }
}

Pool::~Pool() {
  {
    const std::lock_guard<std::mutex> lock(impl_->m);
    impl_->stop = true;
  }
  impl_->wake.notify_all();
  for (std::thread& t : impl_->threads) {
    t.join();
  }
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

  const size_t chunks = (n - 1) / grain + 1;
  const int participants =
      int(std::min<size_t>(s.threads.size(), chunks - 1));
#ifdef PRMD_PROFILE
  void* const scope = profile::detail::current_scope();
#endif
  {
    const std::lock_guard<std::mutex> lock(s.m);
    s.fn = &fn;
    s.n = n;
    s.grain = grain;
    s.fail_chunk = SIZE_MAX;
    s.fail = nullptr;
    s.next.store(0, std::memory_order_relaxed);
    s.cancel.store(false, std::memory_order_relaxed);
    s.participants = participants;
    s.pending = participants;
    ++s.generation;
  }
  s.wake.notify_all();

  s.work();  // the caller takes chunks too

  std::exception_ptr fail;
  {
    std::unique_lock<std::mutex> lock(s.m);
    s.done.wait(lock, [&] { return s.pending == 0; });
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
                   int width,
                   const std::function<void(int, int)>& fn) {
  if (h <= 0) {
    return;
  }
  if (!pool) {
    fn(0, h);
    return;
  }
  const size_t rows =
      std::max<size_t>(1, kChunkSamples / size_t(std::max(width, 1)));
  pool->run(size_t(h), rows,
            [&](size_t b, size_t e) { fn(int(b), int(e)); });
}

void parallel_samples(Pool* pool, size_t n, const Pool::Fn& fn) {
  parallel_for(pool, n, kChunkSamples, fn);
}

}  // namespace prmd::internal
