// .prmd encoder (design.md §3–§5).
//
// Threads (design.md §1.4): with more than one thread, one Pool lives for the
// duration of encode_prmd. Every stage splits pure work over it (rows of a
// level, runs of samples, code-blocks) and each piece writes its own slot,
// so the file is byte-identical for every thread count. With one thread
// there is no pool and each helper runs its loop once on the calling thread.

#include "prmd/internal/prmd_codec.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/parallel.h"
#include "prmd/internal/planes.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/internal/timing.h"
#include "prmd/internal/transform.h"
#include "prmd/profile.h"

namespace prmd::internal {

namespace {

// Planes of one level: one Region per channel, or per (band, channel) in
// band-major order for detail levels.
using Planes = std::vector<Region>;

// The exact (lossless) pyramid before anything is stored.
struct Decomposition {
  std::vector<Planes> levels;              // [level][channel]
  std::vector<std::vector<Planes>> bands;  // [level][channel][band]; haar, cdf53 only
};

// Encoded blocks and their index.
struct EncodedBlocks {
  std::vector<uint32_t> offsets;  // total_blocks + 1 entries
  // One payload per present block, in block-id (= file) order.
  std::vector<std::vector<uint8_t>> payloads;
  uint64_t data_bytes = 0;  // sum of the payload sizes
  std::array<uint64_t, kMaxLevels> level_bytes{};
};

// Per-thread coder scratch above this capacity is released after use (huge
// code-blocks), not kept for the thread's lifetime.
constexpr size_t kMaxKeptScratch = size_t(8) << 20;

// Relative serial cost per level-0 sample and channel, in 1/kWeightOne of a
// gray haar + zstd-1 sample (the work unit of parallel.h, auto_threads).
// Measured at one thread on centre crops of test_image.jpg, 14-core host
// (docs/benchmarks.md, "Automatic thread count"); it only needs to be right
// within a factor of about 1.5, as the automatic count follows the square
// root of the work.
constexpr unsigned kWeightOne = 16;

unsigned work_weight(const EncodeOptions& o) {
  unsigned w = 0;
  switch (o.transform) {
    case Transform::Haar:
      w = 16;
      break;
    case Transform::Cdf53:
      w = 20;
      break;
    case Transform::Laplacian:
      w = o.downsampler == Downsampler::Gauss ? 40 : 30;
      break;
    case Transform::Independent:
      w = o.downsampler == Downsampler::Gauss ? 30 : 23;
      break;
  }
  if (prmd::channels(o.color) == 3) {
    w += 4;  // YCoCg-R, and the colour conversion of a gray source
  }
  if (o.quant > 0) {
    w += 6;
  }
  switch (o.entropy) {
    case Entropy::Range:
      w += 85;  // ~65 (RGB) to ~100 (gray)
      break;
    case Entropy::Raw:
      break;
    case Entropy::Zstd: {
      // Extra cost of zstd level L over level 1 (measured at 1, 2, 3, 5, 7,
      // 9, 12, 15, 17, 19, 22; RGB about twice gray at the high levels).
      static const unsigned kZstdExtra[23] = {
          0,   0,   0,   2,   6,   18,  20,  22,  26,  40,  46,  52,
          60,  90,  120, 150, 190, 230, 300, 400, 500, 700, 1200};
      const int level = o.entropy_level == 0 ? 1 : o.entropy_level;
      w += kZstdExtra[std::clamp(level, 0, 22)];
      break;
    }
  }
  return w;
}

Samples extract(const Region& plane, const Rect& r) {
  Samples out = uninitialized_samples(size_t(r.w) * size_t(r.h));
  for (int y = 0; y < r.h; ++y) {
    const int32_t* src = plane.row(r.y + y) + (r.x - plane.rect.x);
    std::memcpy(out.data() + size_t(y) * size_t(r.w), src,
                size_t(r.w) * sizeof(int32_t));
  }
  return out;
}

// YCoCg-R on planes[first .. first+2]; no-op unless `ycocg` is set and there
// are 3 channels.
void color_forward(Planes& p,
                   size_t first,
                   int channels,
                   bool ycocg,
                   Pool* pool) {
  if (ycocg && channels == 3) {
    int32_t* c0 = p[first].v.data();
    int32_t* c1 = p[first + 1].v.data();
    int32_t* c2 = p[first + 2].v.data();
    parallel_samples(pool, p[first].v.size(), [&](size_t b, size_t e) {
      ycocg_r_forward(c0 + b, c1 + b, c2 + b, e - b);
    });
  }
}

// Quantizes the `channels` planes starting at `first`. Step <= 1 is the
// identity (lossless), so the loop is skipped.
void quantize_planes(Planes& p,
                     size_t first,
                     int channels,
                     int step,
                     Pool* pool) {
  if (step <= 1) {
    return;
  }
  for (int c = 0; c < channels; ++c) {
    int32_t* v = p[first + size_t(c)].v.data();
    parallel_samples(pool, p[first + size_t(c)].v.size(),
                     [&](size_t b, size_t e) {
                       for (size_t i = b; i < e; ++i) {
                         v[i] = quantize(v[i], step);
                       }
                     });
  }
}

// 1. Level planes and detail bands of the exact pyramid, from the level-0
// planes. `wavelet` is null when the levels are plain downsamples and no
// bands are needed: for Transform::Independent, and for
// Transform::Laplacian, whose residuals store_laplacian_residuals takes
// against the reconstructed coarser level.
Decomposition decompose(Planes level0,
                        int num_levels,
                        const LevelTransform* wavelet,
                        Downsampler down,
                        Pool* pool) {
  const int channels = int(level0.size());
  Decomposition out;
  out.levels.resize(size_t(num_levels));
  out.bands.resize(size_t(num_levels));
  out.levels[0] = std::move(level0);

  for (int level = 0; level + 1 < num_levels; ++level) {
    out.bands[size_t(level)].resize(size_t(channels));
    for (int c = 0; c < channels; ++c) {
      const Region& fine = out.levels[size_t(level)][size_t(c)];
      Region coarse;
      if (wavelet) {
        PRMD_TIMED("forward");
        wavelet->forward(fine, coarse, out.bands[size_t(level)][size_t(c)],
                         pool);
      } else {
        PRMD_TIMED("downsample");
        coarse = downsample(fine, down, pool);
      }
      out.levels[size_t(level + 1)].push_back(std::move(coarse));
    }
  }
  return out;
}

// Haar / Cdf53: each detail level stores its bands, band-major, after
// YCoCg-R and quantization (open loop). The bands are moved out of `pyr`.
void store_wavelet_bands(Decomposition& pyr,
                         const LevelTransform& xf,
                         int channels,
                         int quant,
                         bool ycocg,
                         std::vector<Planes>& stored,
                         Pool* pool) {
  const int num_levels = int(pyr.levels.size());
  for (int level = 0; level + 1 < num_levels; ++level) {
    Planes& dst = stored[size_t(level)];
    for (int b = 0; b < xf.num_bands(); ++b) {
      {
        PRMD_TIMED("move_bands");
        for (int c = 0; c < channels; ++c) {
          dst.push_back(
              std::move(pyr.bands[size_t(level)][size_t(c)][size_t(b)]));
        }
      }
      const size_t first = size_t(b * channels);
      {
        PRMD_TIMED("ycocg_forward");
        color_forward(dst, first, channels, ycocg, pool);
      }
      {
        PRMD_TIMED("quantize");
        quantize_planes(dst, first, channels,
                        band_step(xf.band_kind(b), quant), pool);
      }
    }
    Planes().swap(pyr.levels[size_t(level)]);  // no longer needed
  }
}

// Laplacian, closed loop from the coarsest level down: residuals are taken
// against the *reconstructed* coarser level (exactly what the decoder will
// have), so quantization errors never accumulate. Consumes the exact detail
// levels of `pyr` (the residual is computed in their place).
void store_laplacian_residuals(Decomposition& pyr,
                               const std::vector<LevelGeom>& geom,
                               Downsampler down,
                               int channels,
                               int quant,
                               bool ycocg,
                               std::vector<Planes>& stored,
                               Pool* pool) {
  const int num_levels = int(pyr.levels.size());
  const int step = band_step(Band::Residual, quant);
  const bool color = ycocg && channels == 3;
  Planes recon = pyr.levels[size_t(num_levels - 1)];  // base is lossless

  for (int level = num_levels - 2; level >= 0; --level) {
    const Dims fd{geom[size_t(level)].w, geom[size_t(level)].h};

    // Prediction U(recon): the decoder's inverse() with a zero residual.
    Planes pred(static_cast<size_t>(channels));
    {
      PRMD_TIMED("predict_upsample");
      for (int c = 0; c < channels; ++c) {
        laplacian_upsample(down, recon[size_t(c)], fd, pred[size_t(c)], pool);
      }
    }

    // Per sample, in one pass: residual = exact − U(recon) → YCoCg-R →
    // quantize (what is stored); then reconstruct exactly as the decoder
    // will, dequantize → inverse YCoCg-R → + U(recon), into `pred`.
    Planes res = std::move(pyr.levels[size_t(level)]);
    {
      PRMD_TIMED("residual_reconstruct");
      int32_t* r[3] = {};
      int32_t* p[3] = {};
      for (int c = 0; c < channels; ++c) {
        r[c] = res[size_t(c)].v.data();
        p[c] = pred[size_t(c)].v.data();
      }
      parallel_samples(pool, res[0].v.size(), [&](size_t b, size_t e) {
        for (int c = 0; c < channels; ++c) {
          for (size_t i = b; i < e; ++i) {
            r[c][i] -= p[c][i];
          }
        }
        if (color) {
          ycocg_r_forward(r[0] + b, r[1] + b, r[2] + b, e - b);
        }
        if (step > 1) {
          for (int c = 0; c < channels; ++c) {
            for (size_t i = b; i < e; ++i) {
              r[c][i] = quantize(r[c][i], step);
            }
          }
        }
        if (!color && step <= 1) {
          for (int c = 0; c < channels; ++c) {
            for (size_t i = b; i < e; ++i) {
              p[c][i] += r[c][i];
            }
          }
          return;
        }
        constexpr size_t kRun = 1024;
        int32_t t[3][kRun];
        for (size_t i0 = b; i0 < e; i0 += kRun) {
          const size_t len = std::min(kRun, e - i0);
          for (int c = 0; c < channels; ++c) {
            for (size_t k = 0; k < len; ++k) {
              t[c][k] = dequantize(r[c][i0 + k], step);
            }
          }
          if (color) {
            ycocg_r_inverse(t[0], t[1], t[2], len);
          }
          for (int c = 0; c < channels; ++c) {
            for (size_t k = 0; k < len; ++k) {
              p[c][i0 + k] += t[c][k];
            }
          }
        }
      });
    }
    stored[size_t(level)] = std::move(res);
    recon = std::move(pred);
  }
}

// 2. What is actually stored, as whole planes: stored[level][band*C + c].
//    Base levels: pixels (YCoCg-R for RGB unless off); MED, unless off, is
//    applied per block later.
//    Detail levels: bands → YCoCg-R (RGB, unless off) → quantize. Both steps are per
//    sample, so doing them on whole planes is identical to doing them per
//    block.
// Consumes `pyr`.
std::vector<Planes> stored_planes(Decomposition& pyr,
                                  const std::vector<LevelGeom>& geom,
                                  const LevelTransform* xf,
                                  const EncodeOptions& opts,
                                  int channels,
                                  Pool* pool) {
  const int num_levels = int(pyr.levels.size());
  std::vector<Planes> stored(static_cast<size_t>(num_levels));

  if (opts.transform == Transform::Laplacian) {
    // First: it starts from the exact base level, moved out below.
    PRMD_TIMED("laplacian_residuals");
    store_laplacian_residuals(pyr, geom, opts.downsampler, channels,
                              opts.quant, opts.ycocg, stored, pool);
  }

  for (int level = num_levels - 1; level >= 0; --level) {
    if (geom[size_t(level)].base) {
      PRMD_TIMED("move_base");
      stored[size_t(level)] = std::move(pyr.levels[size_t(level)]);
      color_forward(stored[size_t(level)], 0, channels, opts.ycocg, pool);
    }
  }

  if (xf && opts.transform != Transform::Laplacian) {
    PRMD_TIMED("wavelet_bands");
    store_wavelet_bands(pyr, *xf, channels, opts.quant, opts.ycocg, stored,
                        pool);
  }
  return stored;
}

// Samples of block (bx, by): for base levels the pixels, as MED residuals
// when `med` is set; for detail levels the band samples under the block.
BlockPlanes make_block(const Planes& stored,
                       const LevelGeom& g,
                       const LevelTransform* xf,
                       int cblk,
                       int channels,
                       bool med,
                       int bx,
                       int by) {
  const Rect pixels = block_pixel_rect(g, cblk, bx, by);
  BlockPlanes bp;
  bp.channels = channels;

  if (g.base) {
    bp.w = pixels.w;
    bp.h = pixels.h;
    for (int c = 0; c < channels; ++c) {
      Samples px;
      {
        PRMD_TIMED("extract");
        px = extract(stored[size_t(c)], pixels);
      }
      if (med) {
        PRMD_TIMED("med_residual");
        Samples res = uninitialized_samples(px.size());
        med_residual(px.data(), pixels.w, pixels.h, res.data());
        px.swap(res);
      }
      bp.planes.push_back(std::move(px));
      bp.kinds.push_back(Band::Base);
    }
    return bp;
  }

  const Rect samples = xf->band_rect_of_block(pixels, Dims{g.w, g.h});
  bp.w = samples.w;
  bp.h = samples.h;
  PRMD_TIMED("extract");
  for (int b = 0; b < xf->num_bands(); ++b) {
    for (int c = 0; c < channels; ++c) {
      bp.planes.push_back(extract(stored[size_t(b * channels + c)], samples));
      bp.kinds.push_back(xf->band_kind(b));
    }
  }
  return bp;
}

// Coder scratch of the calling thread: coders append to (and may
// over-allocate) this buffer, the payload keeps an exact-size copy.
std::vector<uint8_t>& block_scratch() {
  thread_local std::vector<uint8_t> scratch;
  return scratch;
}

// Trims this thread's scratch after a block (also when the coder throws):
// buffers that a huge code-block grew past kMaxKeptScratch are released.
struct TrimBlockScratch {
  TrimBlockScratch() = default;
  TrimBlockScratch(const TrimBlockScratch&) = delete;
  TrimBlockScratch& operator=(const TrimBlockScratch&) = delete;
  ~TrimBlockScratch() {
    std::vector<uint8_t>& s = block_scratch();
    if (s.capacity() > kMaxKeptScratch) {
      std::vector<uint8_t>().swap(s);
    }
  }
};

// Releases the calling thread's coder scratch when encode_blocks returns or
// throws: after an encode the calling thread keeps no scratch (workers'
// thread_local buffers end with their threads).
struct ReleaseScratch {
  ReleaseScratch() = default;
  ReleaseScratch(const ReleaseScratch&) = delete;
  ReleaseScratch& operator=(const ReleaseScratch&) = delete;
  ~ReleaseScratch() {
    std::vector<uint8_t>().swap(block_scratch());
    release_coder_scratch();
  }
};

// 3. Entropy-codes every present block into its own exact-size payload.
// Blocks are pure functions of the stored planes, so they are coded in any
// order on any thread; payloads and offsets are then laid out in block-id
// order (coarsest level first). Ids of absent edge blocks get empty
// payloads.
//
// Failures behave as in the serial encoder, which codes blocks in id order
// and stops at the first that throws or that takes the data past
// `max_data_bytes`. Workers stop taking blocks as soon as one block failed or
// the payloads finished so far exceed the limit (so a huge input does not
// hold every payload before it fails); then the blocks are walked in id
// order, coding any skipped one on this thread, and the first failure in
// that order is thrown: the same exception, or the same "exceeds 4 GiB"
// error, at every thread count.
EncodedBlocks encode_blocks(const std::vector<Planes>& stored,
                            const std::vector<LevelGeom>& geom,
                            const LevelTransform* xf,
                            const BlockCoder& coder,
                            uint32_t total_blocks,
                            int cblk,
                            int channels,
                            bool med,
                            uint64_t max_data_bytes,
                            Pool* pool) {
  const ReleaseScratch release_scratch;
  const int num_levels = int(geom.size());

  struct Job {
    int level;
    int bx;
    int by;
    uint32_t id;
  };
  std::vector<Job> jobs;
  for (int level = num_levels - 1; level >= 0; --level) {
    const LevelGeom& g = geom[size_t(level)];
    const int k2 = g.k * g.k;
    for (uint32_t local = 0; local < g.num_ids; ++local) {
      // Inverse of block_id(): tile-major, then row-major inside the tile.
      const int tile = int(local) / k2;
      const int in_tile = int(local) % k2;
      const int bx = (tile % g.tiles_x) * g.k + in_tile % g.k;
      const int by = (tile / g.tiles_x) * g.k + in_tile / g.k;
      if (bx < g.blocks_x && by < g.blocks_y) {
        jobs.push_back(Job{level, bx, by, g.first_block + local});
      }
    }
  }

  EncodedBlocks out;
  out.payloads.resize(jobs.size());
  // Per job, written only by the thread that codes it: 1 = coded.
  std::vector<uint8_t> coded(jobs.size(), 0);

  const auto code = [&](size_t i) {
    const TrimBlockScratch trim;
    const Job& j = jobs[i];
    BlockPlanes bp;
    {
      PRMD_TIMED("make_block");
      bp = make_block(stored[size_t(j.level)], geom[size_t(j.level)], xf,
                      cblk, channels, med, j.bx, j.by);
    }
    PRMD_TIMED("entropy_encode");
    std::vector<uint8_t>& scratch = block_scratch();
    scratch.clear();
    coder.encode(bp, scratch);
    out.payloads[i].assign(scratch.begin(), scratch.end());
    coded[i] = 1;
  };

  // Set once a block failed or the coded payloads exceed the limit: the
  // blocks not started yet are skipped.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> coded_bytes{0};
  std::mutex fail_m;
  size_t fail_index = SIZE_MAX;  // lowest job that threw; guarded by fail_m
  std::exception_ptr fail;

  parallel_for(pool, jobs.size(), 1, [&](size_t b, size_t e) {
    for (size_t i = b; i < e; ++i) {
      if (stop.load(std::memory_order_relaxed)) {
        return;
      }
      try {
        code(i);
      } catch (...) {
        const std::lock_guard<std::mutex> lock(fail_m);
        if (i < fail_index) {
          fail_index = i;
          fail = std::current_exception();
        }
        stop.store(true, std::memory_order_relaxed);
        return;
      }
      const uint64_t size = out.payloads[i].size();
      if (coded_bytes.fetch_add(size, std::memory_order_relaxed) + size >
          max_data_bytes) {
        stop.store(true, std::memory_order_relaxed);
      }
    }
  });

  // Index, in id order: the serial encoder's order of coding and checking.
  out.offsets.assign(size_t(total_blocks) + 1, 0);
  uint64_t pos = 0;
  size_t ji = 0;
  for (int level = num_levels - 1; level >= 0; --level) {
    const LevelGeom& g = geom[size_t(level)];
    const uint64_t level_start = pos;
    for (uint32_t local = 0; local < g.num_ids; ++local) {
      const uint32_t id = g.first_block + local;
      if (ji < jobs.size() && jobs[ji].id == id) {
        if (ji == fail_index) {
          std::rethrow_exception(fail);
        }
        if (!coded[ji]) {
          code(ji);  // skipped after a later failure: code it now
        }
        pos += out.payloads[ji].size();
        ++ji;
        if (pos > max_data_bytes) {
          throw std::runtime_error("encoded data exceeds 4 GiB");
        }
      }
      out.offsets[size_t(id) + 1] = uint32_t(pos);
    }
    out.level_bytes[size_t(level)] = pos - level_start;
  }
  out.data_bytes = pos;
  return out;
}

PrmdHeader make_header(const EncodeOptions& opts,
                       int width,
                       int height,
                       int channels,
                       uint32_t total_blocks) {
  PrmdHeader h;
  h.transform = opts.transform;
  h.downsampler = opts.downsampler;
  h.width = uint32_t(width);
  h.height = uint32_t(height);
  h.levels = uint8_t(opts.levels);
  h.color = opts.color;
  h.tile = uint16_t(opts.tile);
  h.cblk = uint16_t(opts.cblk);
  h.quant = uint16_t(opts.quant);
  h.total_blocks = total_blocks;
  h.entropy = opts.entropy;
  h.entropy_level = uint8_t(opts.entropy_level);
  h.med = opts.med;
  // Gray has no color transform; recording "off" would give one image two
  // encodings.
  h.ycocg = opts.ycocg || channels != 3;
  return h;
}

std::vector<LevelEntry> level_table(const std::vector<LevelGeom>& geom) {
  std::vector<LevelEntry> table;
  for (const LevelGeom& g : geom) {
    table.push_back(LevelEntry{uint32_t(g.w), uint32_t(g.h),
                               uint16_t(g.blocks_x), uint16_t(g.blocks_y),
                               g.first_block});
  }
  return table;
}

// Header + index, then the payloads in order, written as they are (no
// concatenated copy of the data section).
void write_file(const std::string& path,
                const std::vector<uint8_t>& head,
                const std::vector<std::vector<uint8_t>>& payloads) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    throw IoError("cannot create '" + path + "'");
  }
  std::setvbuf(f, nullptr, _IOFBF, size_t(1) << 20);
  bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size();
  for (const auto& p : payloads) {
    if (!ok) {
      break;
    }
    ok = p.empty() || std::fwrite(p.data(), 1, p.size(), f) == p.size();
  }
  const bool closed = std::fclose(f) == 0;
  if (!ok || !closed) {
    throw IoError("write failed for '" + path + "'");
  }
}

}  // namespace

void validate_prmd_options(const EncodeOptions& o) {
  if (o.levels < 1 || o.levels > kMaxLevels) {
    throw std::invalid_argument("levels must be in 1..16");
  }
  if (o.cblk < 16 || o.cblk % 2 || o.cblk > 4096) {
    throw std::invalid_argument("cblk must be even, 16..4096");
  }
  if (o.tile < o.cblk || o.tile % o.cblk || o.tile > 32768) {
    throw std::invalid_argument("tile must be a multiple of cblk, <= 32768");
  }
  if (o.quant < 0 || o.quant > 4096) {
    throw std::invalid_argument("quant must be 0 (lossless) .. 4096");
  }
  if (o.quant > 0 && o.transform == Transform::Independent) {
    throw std::invalid_argument(
        "lossy (quant > 0) needs transform haar, cdf53 or laplacian; naive is "
        "lossless only");
  }
  if (uint8_t(o.transform) > uint8_t(Transform::Independent)) {
    throw std::invalid_argument("bad transform");
  }
  if (uint8_t(o.downsampler) > uint8_t(Downsampler::Gauss)) {
    throw std::invalid_argument("bad downsampler");
  }
  (void)make_block_coder(o.entropy, o.entropy_level);  // entropy + level
  (void)info(o.color);
  validate_threads(o.threads);
}

uint64_t prmd_work_units(int width, int height, const EncodeOptions& o) {
  if (width <= 0 || height <= 0) {
    return 0;
  }
  return uint64_t(width) * uint64_t(height) *
         uint64_t(prmd::channels(o.color)) * uint64_t(work_weight(o)) /
         kWeightOne;
}

EncodeReport encode_prmd(const Image& src,
                         const std::string& path,
                         const EncodeOptions& opts,
                         const BlockCoder* coder_override,
                         uint64_t max_data_bytes) {
  PRMD_TIMED("encode_prmd");
  const auto start = Clock::now();
  validate_prmd_options(opts);
  if (src.width <= 0 || src.height <= 0) {
    throw std::invalid_argument("empty source image");
  }
  if (const char* why =
          geometry_error(uint64_t(src.width), uint64_t(src.height),
                         opts.levels, opts.tile, opts.cblk)) {
    throw std::invalid_argument(std::string("image geometry: ") + why);
  }
  const int channels = prmd::channels(opts.color);

  std::unique_ptr<LevelTransform> xf;
  if (opts.transform != Transform::Independent) {
    xf = make_transform(opts.transform, opts.downsampler);
  }

  const auto geom = make_geometry(src.width, src.height, opts.levels,
                                  opts.tile, opts.cblk, opts.transform);
  uint32_t total_blocks = 0;
  uint64_t present_blocks = 0;
  for (const LevelGeom& g : geom) {
    total_blocks += g.num_ids;
    present_blocks += uint64_t(g.blocks_x) * uint64_t(g.blocks_y);
  }

  // Threads: `opts.threads`, or for 0 as many as the amount of work makes
  // worthwhile (auto_threads), never more than there are blocks, the unit of
  // the dominant stage (an image with one block gets no pool). The
  // automatic count is also capped by the level-0 blocks: they are most of
  // the coding, and a block cannot be split, so for a small image with fewer
  // level-0 blocks than threads (a 128² image has one) the extra threads
  // cost more to start than they save (with the slow coders too). One
  // thread: no pool, every stage runs inline on this thread.
  uint64_t max_useful = std::max<uint64_t>(present_blocks, 1);
  if (opts.threads == 0) {
    max_useful = std::min<uint64_t>(
        max_useful,
        std::max<uint64_t>(uint64_t(geom[0].blocks_x) * uint64_t(geom[0].blocks_y),
                           1));
  }
  const int threads = int(std::min<uint64_t>(
      uint64_t(choose_threads(opts.threads,
                              prmd_work_units(src.width, src.height, opts))),
      max_useful));
  std::unique_ptr<Pool> pool_owner;
  if (threads > 1) {
    pool_owner = std::make_unique<Pool>(threads);
  }
  Pool* pool = pool_owner.get();
  const int threads_used = pool ? pool->size() : 1;

  // Level 0 planes straight from the source, converted to opts.color.
  Planes level0;
  {
    PRMD_TIMED("convert");
    level0 = to_planes(src, opts.color, pool);
  }

  // Laplacian levels are the same downsample its forward() would compute;
  // its forward residuals would be discarded, so they are not computed.
  const LevelTransform* wavelet =
      opts.transform == Transform::Laplacian ? nullptr : xf.get();
  Decomposition pyr;
  {
    PRMD_TIMED("decompose");
    pyr = decompose(std::move(level0), opts.levels, wavelet, opts.downsampler,
                    pool);
  }
  std::vector<Planes> stored;
  {
    PRMD_TIMED("stored_planes");
    stored = stored_planes(pyr, geom, xf.get(), opts, channels, pool);
  }

  std::unique_ptr<BlockCoder> own_coder;
  if (!coder_override) {
    own_coder = make_block_coder(opts.entropy, opts.entropy_level);
  }
  const BlockCoder& coder = coder_override ? *coder_override : *own_coder;
  EncodedBlocks blocks;
  {
    PRMD_TIMED("encode_blocks");
    blocks = encode_blocks(stored, geom, xf.get(), coder, total_blocks,
                           opts.cblk, channels, opts.med, max_data_bytes, pool);
  }
  std::vector<Planes>().swap(stored);
  pool_owner.reset();  // all parallel work is done

  PrmdHeader header =
      make_header(opts, src.width, src.height, channels, total_blocks);
  std::vector<uint8_t> head;
  {
    PRMD_TIMED("serialize");
    head = serialize(header, level_table(geom), blocks.offsets);
  }
  {
    PRMD_TIMED("write_file");
    write_file(path, head, blocks.payloads);
  }

  EncodeReport report;
  report.level_bytes = blocks.level_bytes;
  report.file_bytes = head.size() + blocks.data_bytes;
  report.encode_ms = elapsed_ms(start);
  report.threads = threads_used;
  return report;
}

}  // namespace prmd::internal
