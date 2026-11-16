// PRMD_TIMED profiler (include/prmd/profile.h) with the thread pool: scopes
// opened on pool workers are merged into the calling thread's tree, so the
// per-step call counts and times of a multithreaded encode are complete.
// Built against a core compiled with PRMD_PROFILE (prmd_profile_tests).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

#include "prmd/encoder.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/parallel.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/profile.h"
#include "reference.h"
#include "test_util.h"

using namespace prmd;
using prmd::internal::Pool;

static_assert(profile::kEnabled, "prmd_profile_tests needs PRMD_PROFILE");

namespace {

std::map<std::string, profile::Entry> by_path() {
  std::map<std::string, profile::Entry> m;
  for (const auto& e : profile::snapshot()) {
    m[e.path] = e;
  }
  return m;
}

void busy_for(std::chrono::microseconds d) {
  const auto end = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < end) {
  }
}

}  // namespace

TEST(ProfileThreads, WorkerScopesAreMergedUnderTheCallersScope) {
  Pool pool(4);
  profile::reset();
  {
    PRMD_TIMED("outer");
    pool.run(64, 1, [](size_t, size_t) {
      PRMD_TIMED("chunk");
      {
        PRMD_TIMED("inner");
        busy_for(std::chrono::microseconds(500));
      }
    });
  }
  const auto m = by_path();
  ASSERT_EQ(m.count("outer"), 1u);
  ASSERT_EQ(m.count("outer/chunk"), 1u);
  ASSERT_EQ(m.count("outer/chunk/inner"), 1u);
  EXPECT_EQ(m.size(), 3u);  // no stray roots from the workers

  const auto& outer = m.at("outer");
  const auto& chunk = m.at("outer/chunk");
  const auto& inner = m.at("outer/chunk/inner");
  EXPECT_EQ(outer.calls, 1u);
  EXPECT_EQ(outer.worker_calls, 0u);
  EXPECT_EQ(outer.worker_ms, 0.0);  // wall time on the calling thread
  // Every chunk counted, whichever thread ran it.
  EXPECT_EQ(chunk.calls, 64u);
  EXPECT_EQ(inner.calls, 64u);
  EXPECT_GT(chunk.worker_calls, 0u);
  EXPECT_LE(chunk.worker_calls, 64u);
  EXPECT_EQ(inner.worker_calls, chunk.worker_calls);
  EXPECT_GT(chunk.worker_ms, 0.0);
  EXPECT_LE(chunk.worker_ms, chunk.ms);
  // Summed thread time: at least 64 × 0.5 ms, and more than the parent's
  // wall time once several threads worked.
  EXPECT_GE(inner.ms, 64 * 0.5);
  EXPECT_GE(chunk.ms, inner.ms);
  EXPECT_GT(chunk.ms, outer.ms);
}

TEST(ProfileThreads, SerialPoolRecordsOnTheCallerOnly) {
  Pool pool(1);
  profile::reset();
  {
    PRMD_TIMED("outer");
    pool.run(10, 1, [](size_t b, size_t e) {
      for (size_t i = b; i < e; ++i) {
        PRMD_TIMED("item");
      }
    });
  }
  const auto m = by_path();
  EXPECT_EQ(m.at("outer/item").calls, 10u);
  EXPECT_EQ(m.at("outer/item").worker_calls, 0u);
  EXPECT_EQ(m.at("outer/item").worker_ms, 0.0);
}

TEST(ProfileThreads, RunsAccumulateAndResetClearsWorkerTime) {
  Pool pool(3);
  for (int round = 0; round < 3; ++round) {
    profile::reset();
    {
      PRMD_TIMED("outer");
      for (int k = 0; k < 2; ++k) {
        pool.run(30, 1, [](size_t, size_t) {
          PRMD_TIMED("item");
          busy_for(std::chrono::microseconds(100));
        });
      }
    }
    const auto m = by_path();
    EXPECT_EQ(m.at("outer/item").calls, 60u) << round;
    EXPECT_EQ(m.size(), 2u);
  }
}

TEST(ProfileThreads, ScopesOpenedOnlyOnWorkersStillAppear) {
  // A scope the calling thread never opens itself.
  Pool pool(4);
  const auto caller = std::this_thread::get_id();
  std::atomic<int> on_workers{0};
  profile::reset();
  {
    PRMD_TIMED("outer");
    pool.run(40, 1, [&](size_t, size_t) {
      busy_for(std::chrono::microseconds(300));
      if (std::this_thread::get_id() != caller) {
        PRMD_TIMED("worker_only");
        on_workers.fetch_add(1);
      }
    });
  }
  const auto m = by_path();
  ASSERT_GT(on_workers.load(), 0);
  ASSERT_EQ(m.count("outer/worker_only"), 1u);
  const auto& e = m.at("outer/worker_only");
  EXPECT_EQ(e.calls, uint64_t(on_workers.load()));
  EXPECT_EQ(e.worker_calls, e.calls);
  EXPECT_EQ(e.worker_ms, e.ms);
}

namespace {

// zstd coder that also spends 1 ms per block in a timed scope, so that the
// block stage is long enough for every worker to take blocks.
class SlowCoder final : public internal::BlockCoder {
 public:
  SlowCoder() : inner_(internal::make_block_coder(Entropy::Zstd)) {}
  Entropy id() const override { return Entropy::Zstd; }
  void encode(const internal::BlockPlanes& in,
              std::vector<uint8_t>& out) const override {
    {
      PRMD_TIMED("slow");
      busy_for(std::chrono::microseconds(1000));
    }
    inner_->encode(in, out);
  }
  void decode(const uint8_t* d,
              size_t n,
              internal::BlockPlanes& io) const override {
    inner_->decode(d, n, io);
  }

 private:
  std::unique_ptr<internal::BlockCoder> inner_;
};

}  // namespace

TEST(ProfileThreads, MultithreadedEncodeCountsEveryBlock) {
  const Image src = ref::synthetic_image(600, 400, ColorFormat::RGB8, 9);
  EncodeOptions o;
  o.color = ColorFormat::RGB8;
  o.levels = 4;
  o.tile = 64;
  o.cblk = 32;
  int blocks = 0;
  for (const auto& g : internal::make_geometry(src.width, src.height, o.levels,
                                               o.tile, o.cblk, o.transform)) {
    blocks += g.blocks_x * g.blocks_y;
  }
  const std::string path = testutil::temp_path("profile.prmd").string();
  const SlowCoder coder;

  std::map<int, std::map<std::string, profile::Entry>> runs;
  for (const int threads : {1, 4}) {
    o.threads = threads;
    profile::reset();
    const EncodeReport r = internal::encode_prmd(src, path, o, &coder);
    EXPECT_EQ(r.threads, threads);
    runs[threads] = by_path();
  }

  const auto& serial = runs[1];
  const auto& mt = runs[4];
  // Same tree, same call counts: nothing recorded on a worker is lost.
  ASSERT_EQ(mt.size(), serial.size());
  for (const auto& [path_name, e] : serial) {
    ASSERT_EQ(mt.count(path_name), 1u) << path_name;
    EXPECT_EQ(mt.at(path_name).calls, e.calls) << path_name;
    EXPECT_EQ(e.worker_calls, 0u) << path_name;
  }
  const std::string blk = "encode_prmd/encode_blocks/entropy_encode";
  EXPECT_EQ(serial.at(blk).calls, uint64_t(blocks));
  EXPECT_EQ(mt.at(blk + "/slow").calls, uint64_t(blocks));
  EXPECT_GT(mt.at(blk).worker_calls, 0u);
  EXPECT_GT(mt.at(blk).worker_ms, 0.0);
  // The block stage's own scope is wall time; its children sum the threads.
  const auto& stage = mt.at("encode_prmd/encode_blocks");
  EXPECT_EQ(stage.worker_ms, 0.0);
  EXPECT_GE(mt.at(blk + "/slow").ms, blocks * 1.0);
  EXPECT_GT(mt.at(blk).ms, stage.ms);
  EXPECT_LT(stage.ms, serial.at("encode_prmd/encode_blocks").ms);
}
