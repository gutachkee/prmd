// Thread pool (include/prmd/internal/parallel.h): thread-count resolution
// and the automatic count, coverage of [0, n), inline execution,
// lowest-index exception, reuse, nesting, the fewer-threads fallback when
// workers cannot be started, and row stripes on worker threads.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "prmd/internal/parallel.h"

using prmd::internal::auto_threads;
using prmd::internal::choose_threads;
using prmd::internal::kChunkSamples;
using prmd::internal::kMaxThreads;
using prmd::internal::kMinParallelSamples;
using prmd::internal::kAutoWorkGrain;
using prmd::internal::parallel_for;
using prmd::internal::parallel_rows;
using prmd::internal::parallel_samples;
using prmd::internal::Pool;
using prmd::internal::resolve_threads;
using prmd::internal::validate_threads;
namespace test_hooks = prmd::internal::test_hooks;

namespace {

// Runs pool.run(n, grain, ...) and checks every index is visited exactly once
// in chunks [k*grain, min(n, (k+1)*grain)).
void expect_exact_cover(Pool& pool, size_t n, size_t grain) {
  std::vector<std::atomic<int>> hits(n);
  std::atomic<bool> bad_chunk{false};
  const size_t g = grain == 0 ? 1 : grain;
  pool.run(n, grain, [&](size_t b, size_t e) {
    // Inline execution passes the whole range as one call.
    const bool whole = b == 0 && e == n;
    if (!whole && (b % g != 0 || e != std::min(n, b + g) || b >= e)) {
      bad_chunk = true;
    }
    for (size_t i = b; i < e; ++i) {
      hits[i].fetch_add(1);
    }
  });
  EXPECT_FALSE(bad_chunk) << "n=" << n << " grain=" << grain;
  for (size_t i = 0; i < n; ++i) {
    ASSERT_EQ(hits[i].load(), 1) << "index " << i << " n=" << n
                                 << " grain=" << grain;
  }
}

}  // namespace

TEST(ResolveThreads, ZeroMeansHardwareConcurrency) {
  EXPECT_EQ(resolve_threads(0, 8), 8);
  EXPECT_EQ(resolve_threads(0, 1), 1);
  EXPECT_EQ(resolve_threads(0, 0), 1);  // unknown → serial
  EXPECT_EQ(resolve_threads(0, 100000), kMaxThreads);
  const unsigned hw = std::thread::hardware_concurrency();
  EXPECT_EQ(resolve_threads(0),
            hw == 0 ? 1 : std::min<int>(int(hw), kMaxThreads));
}

TEST(ResolveThreads, PositiveValuesAreClamped) {
  EXPECT_EQ(resolve_threads(1, 8), 1);
  EXPECT_EQ(resolve_threads(3, 8), 3);
  EXPECT_EQ(resolve_threads(64, 8), 64);  // more than the hardware is allowed
  EXPECT_EQ(resolve_threads(kMaxThreads, 8), kMaxThreads);
  EXPECT_EQ(resolve_threads(kMaxThreads + 1, 8), kMaxThreads);
  EXPECT_EQ(resolve_threads(INT_MAX, 8), kMaxThreads);
}

TEST(ResolveThreads, NegativeIsRejected) {
  EXPECT_THROW(resolve_threads(-1, 8), std::invalid_argument);
  EXPECT_THROW(resolve_threads(INT_MIN, 8), std::invalid_argument);
  EXPECT_THROW(resolve_threads(-1), std::invalid_argument);
}

TEST(ResolveThreads, ValidateRejectsOnlyNegatives) {
  EXPECT_NO_THROW(validate_threads(0));
  EXPECT_NO_THROW(validate_threads(1));
  EXPECT_NO_THROW(validate_threads(INT_MAX));
  EXPECT_THROW(validate_threads(-1), std::invalid_argument);
  EXPECT_THROW(validate_threads(INT_MIN), std::invalid_argument);
  try {
    validate_threads(-3);
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ(e.what(), "threads must be >= 0 (0 = automatic)");
  }
}

TEST(AutoThreads, NThreadsFromNSquaredGrainsOfWork) {
  // A pure function of (work, hardware): deterministic for every image.
  const uint64_t g = kAutoWorkGrain;
  EXPECT_EQ(auto_threads(0, 14), 1);
  EXPECT_EQ(auto_threads(1, 14), 1);
  EXPECT_EQ(auto_threads(4 * g - 1, 14), 1);  // below 2 threads' worth
  EXPECT_EQ(auto_threads(4 * g, 14), 2);
  EXPECT_EQ(auto_threads(9 * g - 1, 14), 2);
  EXPECT_EQ(auto_threads(9 * g, 14), 3);
  EXPECT_EQ(auto_threads(25 * g + 7, 14), 5);
  EXPECT_EQ(auto_threads(195 * g, 14), 13);
  EXPECT_EQ(auto_threads(196 * g, 14), 14);
  EXPECT_EQ(auto_threads(100000 * g, 14), 14);
  EXPECT_EQ(auto_threads(UINT64_MAX, 14), 14);
  // Never more than the hardware, nor than kMaxThreads; unknown hardware
  // (0) is serial.
  EXPECT_EQ(auto_threads(UINT64_MAX, 1), 1);
  EXPECT_EQ(auto_threads(UINT64_MAX, 0), 1);
  EXPECT_EQ(auto_threads(UINT64_MAX, 100000), kMaxThreads);
  EXPECT_EQ(auto_threads(100 * g, 2), 2);
  // Exact square roots over a wide range, and monotone in the work.
  int prev = 1;
  for (uint64_t w = 0; w < 300 * 300 * g; w += g / 3 + w / 50) {
    const int t = auto_threads(w, 1000);
    uint64_t r = 1;
    while ((r + 1) * (r + 1) * g <= w) {
      ++r;
    }
    ASSERT_EQ(t, int(std::min<uint64_t>(r, kMaxThreads))) << w;
    ASSERT_GE(t, prev) << w;
    prev = t;
  }
}

TEST(AutoThreads, ChooseThreadsTreatsOnlyZeroAsAutomatic) {
  const uint64_t tiny = 10;
  const uint64_t huge = 1000 * 1000 * kAutoWorkGrain;
  EXPECT_EQ(choose_threads(0, tiny, 14), 1);
  EXPECT_EQ(choose_threads(0, huge, 14), 14);
  EXPECT_EQ(choose_threads(0, huge, 14), auto_threads(huge, 14));
  // Explicit counts are honoured whatever the work (an upper bound that
  // callers cap by their units of work), clamped to kMaxThreads.
  EXPECT_EQ(choose_threads(1, huge, 14), 1);
  EXPECT_EQ(choose_threads(5, tiny, 14), 5);
  EXPECT_EQ(choose_threads(64, tiny, 8), 64);
  EXPECT_EQ(choose_threads(INT_MAX, tiny, 8), kMaxThreads);
  EXPECT_THROW(choose_threads(-1, huge, 14), std::invalid_argument);
  const unsigned hw = std::thread::hardware_concurrency();
  EXPECT_EQ(choose_threads(0, huge), auto_threads(huge, hw));
  EXPECT_LE(choose_threads(0, UINT64_MAX), std::max(1, int(hw)));
}

TEST(Pool, SizeCountsTheCaller) {
  EXPECT_EQ(Pool(1).size(), 1);
  EXPECT_EQ(Pool(0).size(), 1);
  EXPECT_EQ(Pool(-5).size(), 1);
  EXPECT_EQ(Pool(4).size(), 4);
  EXPECT_EQ(Pool(kMaxThreads + 10).size(), kMaxThreads);
}

TEST(Pool, SingleThreadRunsInlineAsOneCall) {
  Pool pool(1);
  const auto caller = std::this_thread::get_id();
  int calls = 0;
  pool.run(1000, 7, [&](size_t b, size_t e) {
    ++calls;  // no synchronization needed: same thread
    EXPECT_EQ(b, 0u);
    EXPECT_EQ(e, 1000u);
    EXPECT_EQ(std::this_thread::get_id(), caller);
  });
  EXPECT_EQ(calls, 1);
}

TEST(Pool, SmallJobsRunInlineOnTheCaller) {
  Pool pool(8);
  const auto caller = std::this_thread::get_id();
  for (const size_t n : {size_t(1), size_t(5), size_t(64)}) {
    int calls = 0;
    pool.run(n, 64, [&](size_t b, size_t e) {
      ++calls;
      EXPECT_EQ(b, 0u);
      EXPECT_EQ(e, n);
      EXPECT_EQ(std::this_thread::get_id(), caller);
    });
    EXPECT_EQ(calls, 1) << n;
  }
}

TEST(Pool, ZeroItemsCallsNothing) {
  for (const int threads : {1, 4}) {
    Pool pool(threads);
    bool called = false;
    pool.run(0, 1, [&](size_t, size_t) { called = true; });
    EXPECT_FALSE(called);
  }
  bool called = false;
  parallel_for(nullptr, 0, 1, [&](size_t, size_t) { called = true; });
  parallel_rows(nullptr, 0, 10, [&](int, int) { called = true; });
  EXPECT_FALSE(called);
}

TEST(Pool, EveryIndexExactlyOnce) {
  for (const int threads : {1, 2, 3, 7, 16}) {
    Pool pool(threads);
    for (const size_t n : {size_t(1), size_t(2), size_t(3), size_t(17),
                           size_t(100), size_t(1000), size_t(12345)}) {
      for (const size_t grain : {size_t(0), size_t(1), size_t(3), size_t(64),
                                 size_t(5000)}) {
        SCOPED_TRACE("threads " + std::to_string(threads));
        expect_exact_cover(pool, n, grain);
      }
    }
  }
}

TEST(Pool, MoreThreadsThanItems) {
  Pool pool(16);
  expect_exact_cover(pool, 1, 1);
  expect_exact_cover(pool, 2, 1);
  expect_exact_cover(pool, 3, 1);
  expect_exact_cover(pool, 15, 1);
}

TEST(Pool, WorkersActuallyRunChunks) {
  Pool pool(4);
  std::mutex m;
  std::set<std::thread::id> ids;
  pool.run(64, 1, [&](size_t, size_t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::lock_guard<std::mutex> lock(m);
    ids.insert(std::this_thread::get_id());
  });
  EXPECT_GT(ids.size(), 1u);
  EXPECT_LE(ids.size(), 4u);
}

TEST(Pool, ResultsInIndexSlotsAreIdenticalForEveryThreadCount) {
  auto compute = [](int threads) {
    Pool pool(threads);
    std::vector<uint64_t> out(5000);
    pool.run(out.size(), 13, [&](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        uint64_t h = i * 0x9E3779B97F4A7C15ull;
        for (int k = 0; k < 50; ++k) {
          h ^= h >> 29;
          h *= 0xBF58476D1CE4E5B9ull;
        }
        out[i] = h;
      }
    });
    return out;
  };
  const auto serial = compute(1);
  for (const int threads : {2, 3, 7, 16}) {
    EXPECT_EQ(compute(threads), serial) << threads;
  }
}

TEST(Pool, RethrowsTheLowestFailingIndex) {
  // Failures at 900, 37 and 5: the serial loop throws at 5 first.
  const std::set<size_t> failing = {900, 37, 5};
  for (const int threads : {1, 2, 3, 7, 16}) {
    Pool pool(threads);
    for (int rep = 0; rep < 50; ++rep) {
      std::vector<std::atomic<int>> ran(1000);
      std::mt19937 rng(static_cast<unsigned>(rep));
      std::vector<int> delay_us(1000);
      for (auto& d : delay_us) {
        d = int(rng() % 20);
      }
      try {
        pool.run(1000, 1, [&](size_t b, size_t e) {
          for (size_t i = b; i < e; ++i) {
            if (delay_us[i] == 0) {
              std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (failing.count(i)) {
              throw std::out_of_range("index " + std::to_string(i));
            }
            ran[i].fetch_add(1);
          }
        });
        FAIL() << "no exception";
      } catch (const std::out_of_range& e) {
        EXPECT_STREQ(e.what(), "index 5") << "threads " << threads;
      }
      // Everything before the first failure ran, exactly once.
      for (size_t i = 0; i < 5; ++i) {
        ASSERT_EQ(ran[i].load(), 1) << i;
      }
      for (size_t i = 0; i < 1000; ++i) {
        ASSERT_LE(ran[i].load(), 1) << i;
      }
    }
  }
}

TEST(Pool, LowestFailingIndexInsideChunks) {
  // grain 10: the failure at 23 is in chunk [20, 30), after 21 and 22 ran.
  for (const int threads : {1, 2, 7}) {
    Pool pool(threads);
    try {
      pool.run(200, 10, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
          if (i == 23 || i == 24 || i == 150) {
            throw std::runtime_error("at " + std::to_string(i));
          }
        }
      });
      FAIL() << "no exception";
    } catch (const std::runtime_error& e) {
      EXPECT_STREQ(e.what(), "at 23") << threads;
    }
  }
}

TEST(Pool, ExceptionOnlyAtIndexZero) {
  // Index 0 is always taken by the calling thread first or by a worker; the
  // message must not depend on which.
  Pool pool(8);
  for (int rep = 0; rep < 100; ++rep) {
    try {
      pool.run(64, 1, [&](size_t b, size_t) {
        throw std::runtime_error("chunk " + std::to_string(b));
      });
      FAIL();
    } catch (const std::runtime_error& e) {
      ASSERT_STREQ(e.what(), "chunk 0");
    }
  }
}

TEST(Pool, ReusableAcrossCallsAndAfterAnException) {
  Pool pool(5);
  for (int round = 0; round < 300; ++round) {
    const size_t n = size_t(1 + round * 7 % 500);
    if (round % 50 == 3) {
      EXPECT_THROW(pool.run(n + 2, 1,
                            [&](size_t b, size_t) {
                              if (b == 1) {
                                throw std::logic_error("x");
                              }
                            }),
                   std::logic_error);
      continue;
    }
    std::atomic<uint64_t> sum{0};
    pool.run(n, 3, [&](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        sum.fetch_add(i);
      }
    });
    ASSERT_EQ(sum.load(), n * (n - 1) / 2) << "round " << round;
  }
}

TEST(Pool, NestedRunExecutesInline) {
  Pool pool(4);
  std::vector<std::atomic<int>> hits(40 * 30);
  pool.run(40, 1, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) {
      const auto self = std::this_thread::get_id();
      pool.run(30, 1, [&](size_t b2, size_t e2) {
        EXPECT_EQ(std::this_thread::get_id(), self);
        for (size_t j = b2; j < e2; ++j) {
          hits[i * 30 + j].fetch_add(1);
        }
      });
    }
  });
  for (const auto& h : hits) {
    ASSERT_EQ(h.load(), 1);
  }
}

TEST(Pool, ConstructAndDestroyWithoutWork) {
  for (int i = 0; i < 20; ++i) {
    Pool pool(8);
    EXPECT_EQ(pool.size(), 8);
  }
}

TEST(Pool, FewerThreadsWhenWorkersCannotBeStarted) {
  // The system refusing a thread (std::system_error from std::thread) is
  // simulated after k workers: the pool keeps the ones it has.
  struct Reset {
    ~Reset() { test_hooks::set_worker_start_limit(-1); }
  } reset;
  for (const int k : {0, 1, 3}) {
    test_hooks::set_worker_start_limit(k);
    Pool pool(8);
    EXPECT_EQ(pool.size(), 1 + k) << k;
    for (const size_t n : {size_t(1), size_t(17), size_t(1000)}) {
      expect_exact_cover(pool, n, 3);
    }
    std::mutex m;
    std::set<std::thread::id> ids;
    pool.run(64, 1, [&](size_t, size_t) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const std::lock_guard<std::mutex> lock(m);
      ids.insert(std::this_thread::get_id());
    });
    EXPECT_LE(ids.size(), size_t(1 + k));
    // The lowest-index exception still wins with the reduced pool.
    try {
      pool.run(50, 1, [](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
          if (i == 7 || i == 30) {
            throw std::out_of_range(std::to_string(i));
          }
        }
      });
      ADD_FAILURE() << "no exception";
    } catch (const std::out_of_range& e) {
      EXPECT_STREQ(e.what(), "7");
    }
  }
  test_hooks::set_worker_start_limit(-1);
  EXPECT_EQ(Pool(8).size(), 8);
}

TEST(Pool, OtherStartFailuresJoinTheStartedWorkersAndRethrow) {
  // Not a refused thread (std::system_error) but, e.g., std::bad_alloc
  // after k workers started: the constructor joins them (a joinable
  // std::thread destroyed would call std::terminate) and rethrows.
  struct Reset {
    ~Reset() { test_hooks::set_worker_start_limit(-1); }
  } reset;
  for (const int k : {0, 1, 3}) {
    test_hooks::set_worker_start_limit(
        k, test_hooks::WorkerStartFailure::OutOfMemory);
    EXPECT_THROW(Pool(8), std::bad_alloc) << k;
  }
  // Only a start beyond the limit fails.
  test_hooks::set_worker_start_limit(
      2, test_hooks::WorkerStartFailure::OutOfMemory);
  EXPECT_THROW(Pool(4), std::bad_alloc);
  EXPECT_EQ(Pool(3).size(), 3);
  test_hooks::set_worker_start_limit(-1);
  EXPECT_EQ(Pool(8).size(), 8);
}

TEST(Pool, ManyBackToBackSmallJobs) {
  // Stages follow each other quickly: workers that are spinning or asleep
  // must join (or be skipped) correctly every time.
  Pool pool(6);
  for (int round = 0; round < 3000; ++round) {
    const size_t n = size_t(2 + round % 40);
    std::atomic<uint64_t> sum{0};
    pool.run(n, 1, [&](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        sum.fetch_add(i + 1);
      }
    });
    ASSERT_EQ(sum.load(), n * (n + 1) / 2) << round;
    if (round % 500 == 499) {
      // Let every worker fall asleep, then wake them again.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
}

namespace {

std::mutex g_stripe_m;
std::set<std::thread::id> g_stripe_threads;
std::atomic<int> g_stripes{0};

void record_stripe(int, int) {
  g_stripes.fetch_add(1);
  const std::lock_guard<std::mutex> lock(g_stripe_m);
  g_stripe_threads.insert(std::this_thread::get_id());
}

}  // namespace

TEST(ParallelHelpers, RowStripesRunOnWorkerThreads) {
  struct Reset {
    ~Reset() { test_hooks::set_row_stripe_observer(nullptr); }
  } reset;
  {
    const std::lock_guard<std::mutex> lock(g_stripe_m);
    g_stripe_threads.clear();
  }
  g_stripes = 0;
  test_hooks::set_row_stripe_observer(record_stripe);
  Pool pool(4);
  const int h = 2000;
  const size_t width = 1000;
  std::vector<int> out(size_t(h), 0);
  parallel_rows(&pool, h, width, [&](int y0, int y1) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    for (int y = y0; y < y1; ++y) {
      out[size_t(y)] = y;
    }
  });
  for (int y = 0; y < h; ++y) {
    ASSERT_EQ(out[size_t(y)], y);
  }
  EXPECT_GT(g_stripes.load(), 1);
  EXPECT_GT(g_stripe_threads.size(), 1u);
  EXPECT_LE(g_stripe_threads.size(), 4u);
}

TEST(ParallelHelpers, SmallLoopsRunInline) {
  Pool pool(4);
  const auto caller = std::this_thread::get_id();
  int calls = 0;
  parallel_samples(&pool, kMinParallelSamples, [&](size_t b, size_t e) {
    ++calls;
    EXPECT_EQ(b, 0u);
    EXPECT_EQ(e, kMinParallelSamples);
    EXPECT_EQ(std::this_thread::get_id(), caller);
  });
  parallel_rows(&pool, 32, kMinParallelSamples / 32, [&](int y0, int y1) {
    ++calls;
    EXPECT_EQ(y0, 0);
    EXPECT_EQ(y1, 32);
    EXPECT_EQ(std::this_thread::get_id(), caller);
  });
  EXPECT_EQ(calls, 2);
}

TEST(ParallelHelpers, NullPoolIsOneSerialCall) {
  int calls = 0;
  parallel_for(nullptr, 100, 1, [&](size_t b, size_t e) {
    ++calls;
    EXPECT_EQ(b, 0u);
    EXPECT_EQ(e, 100u);
  });
  parallel_rows(nullptr, 50, 1 << 20, [&](int y0, int y1) {
    ++calls;
    EXPECT_EQ(y0, 0);
    EXPECT_EQ(y1, 50);
  });
  parallel_samples(nullptr, 1u << 20, [&](size_t b, size_t e) {
    ++calls;
    EXPECT_EQ(b, 0u);
    EXPECT_EQ(e, size_t(1) << 20);
  });
  EXPECT_EQ(calls, 3);
}

TEST(ParallelHelpers, RowStripesCoverEveryRowOnce) {
  Pool pool(3);
  for (const int width : {1, 7, 300, int(kChunkSamples), 100000}) {
    for (const int h : {1, 2, 5, 129, 777}) {
      std::vector<std::atomic<int>> hits(static_cast<size_t>(h));
      std::atomic<bool> bad{false};
      const int rows = std::max(1, int(kChunkSamples) / width);
      parallel_rows(&pool, h, width, [&](int y0, int y1) {
        if (y0 >= y1 || (y1 - y0 > rows && !(y0 == 0 && y1 == h))) {
          bad = true;
        }
        for (int y = y0; y < y1; ++y) {
          hits[size_t(y)].fetch_add(1);
        }
      });
      EXPECT_FALSE(bad);
      for (int y = 0; y < h; ++y) {
        ASSERT_EQ(hits[size_t(y)].load(), 1)
            << "width " << width << " h " << h << " row " << y;
      }
    }
  }
}
