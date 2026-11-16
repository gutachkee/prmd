// .prmd encoder (DESIGN.md §3–§5).

#include "prmd/internal/prmd_codec.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
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

// Blocks encoded back to back, plus their index.
struct EncodedBlocks {
  std::vector<uint32_t> offsets;  // total_blocks + 1 entries
  std::vector<uint8_t> data;
  std::array<uint64_t, kMaxLevels> level_bytes{};
};

std::vector<int32_t> extract(const Region& plane, const Rect& r) {
  std::vector<int32_t> out(size_t(r.w) * size_t(r.h));
  for (int y = 0; y < r.h; ++y) {
    const int32_t* src = plane.row(r.y + y) + (r.x - plane.rect.x);
    std::memcpy(out.data() + size_t(y) * size_t(r.w), src,
                size_t(r.w) * sizeof(int32_t));
  }
  return out;
}

// YCoCg-R on planes[first .. first+2]; no-op unless there are 3 channels.
void color_forward(Planes& p, size_t first, int channels) {
  if (channels == 3) {
    ycocg_r_forward(p[first].v.data(), p[first + 1].v.data(),
                    p[first + 2].v.data(), p[first].v.size());
  }
}

void color_inverse(Planes& p, size_t first, int channels) {
  if (channels == 3) {
    ycocg_r_inverse(p[first].v.data(), p[first + 1].v.data(),
                    p[first + 2].v.data(), p[first].v.size());
  }
}

// Quantizes / dequantizes the `channels` planes starting at `first`.
void quantize_planes(Planes& p, size_t first, int channels, int step) {
  for (int c = 0; c < channels; ++c) {
    for (auto& v : p[first + size_t(c)].v) {
      v = quantize(v, step);
    }
  }
}

void dequantize_planes(Planes& p, size_t first, int channels, int step) {
  for (int c = 0; c < channels; ++c) {
    for (auto& v : p[first + size_t(c)].v) {
      v = dequantize(v, step);
    }
  }
}

// 1. Level planes and detail bands of the exact pyramid. `wavelet` is null
// when the levels are plain downsamples and no bands are needed: for
// Transform::Independent, and for Transform::Laplacian, whose residuals
// store_laplacian_residuals takes against the reconstructed coarser level.
Decomposition decompose(const Image& src,
                        int num_levels,
                        const LevelTransform* wavelet,
                        Downsampler down) {
  const int channels = src.channels();
  Decomposition out;
  out.levels.resize(size_t(num_levels));
  out.bands.resize(size_t(num_levels));
  {
    PRMD_TIMED("to_planes");
    out.levels[0] = to_planes(src);
  }

  for (int level = 0; level + 1 < num_levels; ++level) {
    out.bands[size_t(level)].resize(size_t(channels));
    for (int c = 0; c < channels; ++c) {
      const Region& fine = out.levels[size_t(level)][size_t(c)];
      Region coarse;
      if (wavelet) {
        PRMD_TIMED("forward");
        wavelet->forward(fine, coarse, out.bands[size_t(level)][size_t(c)]);
      } else {
        PRMD_TIMED("downsample");
        coarse = downsample(fine, down);
      }
      out.levels[size_t(level + 1)].push_back(std::move(coarse));
    }
  }
  return out;
}

// Haar / Cdf53: each detail level stores its bands, band-major, after
// YCoCg-R and quantization (open loop).
void store_wavelet_bands(const Decomposition& pyr,
                         const LevelTransform& xf,
                         int channels,
                         int quant,
                         std::vector<Planes>& stored) {
  const int num_levels = int(pyr.levels.size());
  for (int level = 0; level + 1 < num_levels; ++level) {
    Planes& dst = stored[size_t(level)];
    for (int b = 0; b < xf.num_bands(); ++b) {
      {
        PRMD_TIMED("copy_bands");
        for (int c = 0; c < channels; ++c) {
          dst.push_back(pyr.bands[size_t(level)][size_t(c)][size_t(b)]);
        }
      }
      const size_t first = size_t(b * channels);
      {
        PRMD_TIMED("ycocg_forward");
        color_forward(dst, first, channels);
      }
      {
        PRMD_TIMED("quantize");
        quantize_planes(dst, first, channels,
                        band_step(xf.band_kind(b), quant));
      }
    }
  }
}

// Laplacian, closed loop from the coarsest level down: residuals are taken
// against the *reconstructed* coarser level (exactly what the decoder will
// have), so quantization errors never accumulate.
void store_laplacian_residuals(const Decomposition& pyr,
                               const std::vector<LevelGeom>& geom,
                               const LevelTransform& xf,
                               int channels,
                               int quant,
                               std::vector<Planes>& stored) {
  const int num_levels = int(pyr.levels.size());
  const int step = band_step(Band::Residual, quant);
  Planes recon = pyr.levels[size_t(num_levels - 1)];  // base is lossless

  for (int level = num_levels - 2; level >= 0; --level) {
    const Rect full{0, 0, geom[size_t(level)].w, geom[size_t(level)].h};
    const Dims fd{full.w, full.h};
    const Planes& exact = pyr.levels[size_t(level)];

    // Residual = exact level − U(recon).
    Planes pred(static_cast<size_t>(channels));
    Planes res(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
      {
        PRMD_TIMED("predict_upsample");
        const Planes zero{Region(full)};
        xf.inverse(recon[size_t(c)], zero, full, fd, pred[size_t(c)]);
      }
      PRMD_TIMED("subtract");
      res[size_t(c)] = Region(full);
      for (size_t i = 0; i < res[size_t(c)].v.size(); ++i) {
        res[size_t(c)].v[i] = exact[size_t(c)].v[i] - pred[size_t(c)].v[i];
      }
    }

    {
      PRMD_TIMED("ycocg_forward");
      color_forward(res, 0, channels);
    }
    {
      PRMD_TIMED("quantize");
      quantize_planes(res, 0, channels, step);
    }
    {
      PRMD_TIMED("copy_residuals");
      stored[size_t(level)] = res;
    }

    // Reconstruct exactly as the decoder will: dequantize → inverse color →
    // U(recon) + residual.
    {
      PRMD_TIMED("dequantize");
      dequantize_planes(res, 0, channels, step);
    }
    {
      PRMD_TIMED("ycocg_inverse");
      color_inverse(res, 0, channels);
    }
    PRMD_TIMED("reconstruct_upsample");
    Planes next(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
      const Planes band{res[size_t(c)]};
      xf.inverse(recon[size_t(c)], band, full, fd, next[size_t(c)]);
    }
    recon = std::move(next);
  }
}

// 2. What is actually stored, as whole planes: stored[level][band*C + c].
//    Base levels: pixels (YCoCg-R for RGB); MED is applied per block later.
//    Detail levels: bands → YCoCg-R (RGB) → quantize. Both steps are per
//    sample, so doing them on whole planes is identical to doing them per
//    block.
std::vector<Planes> stored_planes(const Decomposition& pyr,
                                  const std::vector<LevelGeom>& geom,
                                  const LevelTransform* xf,
                                  const EncodeOptions& opts,
                                  int channels) {
  const int num_levels = int(pyr.levels.size());
  std::vector<Planes> stored(static_cast<size_t>(num_levels));

  for (int level = num_levels - 1; level >= 0; --level) {
    if (geom[size_t(level)].base) {
      PRMD_TIMED("copy_base");
      stored[size_t(level)] = pyr.levels[size_t(level)];
      color_forward(stored[size_t(level)], 0, channels);
    }
  }

  if (!xf) {
    return stored;  // Independent: every level is a base level
  }
  if (opts.transform == Transform::Laplacian) {
    PRMD_TIMED("laplacian_residuals");
    store_laplacian_residuals(pyr, geom, *xf, channels, opts.quant, stored);
  } else {
    PRMD_TIMED("wavelet_bands");
    store_wavelet_bands(pyr, *xf, channels, opts.quant, stored);
  }
  return stored;
}

// Samples of block (bx, by): MED residuals of the pixels for base levels,
// the band samples under the block for detail levels.
BlockPlanes make_block(const Planes& stored,
                       const LevelGeom& g,
                       const LevelTransform* xf,
                       int cblk,
                       int channels,
                       int bx,
                       int by) {
  const Rect pixels = block_pixel_rect(g, cblk, bx, by);
  BlockPlanes bp;
  bp.channels = channels;

  if (g.base) {
    bp.w = pixels.w;
    bp.h = pixels.h;
    for (int c = 0; c < channels; ++c) {
      std::vector<int32_t> px;
      {
        PRMD_TIMED("extract");
        px = extract(stored[size_t(c)], pixels);
      }
      PRMD_TIMED("med_residual");
      std::vector<int32_t> res(px.size());
      med_residual(px.data(), pixels.w, pixels.h, res.data());
      bp.planes.push_back(std::move(res));
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

// 3. Entropy-codes every block, coarsest level first, in block-id order.
// Ids of absent edge blocks get empty payloads.
EncodedBlocks encode_blocks(const std::vector<Planes>& stored,
                            const std::vector<LevelGeom>& geom,
                            const LevelTransform* xf,
                            const BlockCoder& coder,
                            uint32_t total_blocks,
                            int cblk,
                            int channels) {
  EncodedBlocks out;
  out.offsets.assign(size_t(total_blocks) + 1, 0);
  const int num_levels = int(geom.size());

  for (int level = num_levels - 1; level >= 0; --level) {
    const LevelGeom& g = geom[size_t(level)];
    const size_t level_start = out.data.size();
    const int k2 = g.k * g.k;

    for (uint32_t local = 0; local < g.num_ids; ++local) {
      // Inverse of block_id(): tile-major, then row-major inside the tile.
      const int tile = int(local) / k2;
      const int in_tile = int(local) % k2;
      const int bx = (tile % g.tiles_x) * g.k + in_tile % g.k;
      const int by = (tile / g.tiles_x) * g.k + in_tile / g.k;
      const uint32_t id = g.first_block + local;

      if (bx < g.blocks_x && by < g.blocks_y) {
        BlockPlanes bp;
        {
          PRMD_TIMED("make_block");
          bp = make_block(stored[size_t(level)], g, xf, cblk, channels, bx, by);
        }
        PRMD_TIMED("entropy_encode");
        coder.encode(bp, out.data);
        if (out.data.size() > 0xFFFFFFFFull) {
          throw std::runtime_error("encoded data exceeds 4 GiB");
        }
      }
      out.offsets[size_t(id) + 1] = uint32_t(out.data.size());
    }
    out.level_bytes[size_t(level)] = out.data.size() - level_start;
  }
  return out;
}

PrmdHeader make_header(const EncodeOptions& opts,
                       const Image& src,
                       uint32_t total_blocks) {
  PrmdHeader h;
  h.transform = opts.transform;
  h.downsampler = opts.downsampler;
  h.width = uint32_t(src.width);
  h.height = uint32_t(src.height);
  h.levels = uint8_t(opts.levels);
  h.color = opts.color;
  h.tile = uint16_t(opts.tile);
  h.cblk = uint16_t(opts.cblk);
  h.quant = uint16_t(opts.quant);
  h.total_blocks = total_blocks;
  h.entropy = opts.entropy;
  h.entropy_level = uint8_t(opts.entropy_level);
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

void write_file(const std::string& path,
                const std::vector<uint8_t>& head,
                const std::vector<uint8_t>& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    throw IoError("cannot create '" + path + "'");
  }
  const bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
                  std::fwrite(data.data(), 1, data.size(), f) == data.size();
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
}

EncodeReport encode_prmd(const Image& src_in,
                         const std::string& path,
                         const EncodeOptions& opts) {
  PRMD_TIMED("encode_prmd");
  const auto start = Clock::now();
  validate_prmd_options(opts);
  if (src_in.width <= 0 || src_in.height <= 0) {
    throw std::invalid_argument("empty source image");
  }
  if (const char* why =
          geometry_error(uint64_t(src_in.width), uint64_t(src_in.height),
                         opts.levels, opts.tile, opts.cblk)) {
    throw std::invalid_argument(std::string("image geometry: ") + why);
  }

  Image src;
  {
    PRMD_TIMED("convert");
    src = convert(src_in, opts.color);
  }
  const int channels = src.channels();

  std::unique_ptr<LevelTransform> xf;
  if (opts.transform != Transform::Independent) {
    xf = make_transform(opts.transform, opts.downsampler);
  }

  const auto geom = make_geometry(src.width, src.height, opts.levels, opts.tile,
                                  opts.cblk, opts.transform);
  uint32_t total_blocks = 0;
  for (const LevelGeom& g : geom) {
    total_blocks += g.num_ids;
  }

  // Laplacian levels are the same downsample its forward() would compute;
  // its forward residuals would be discarded, so they are not computed.
  const LevelTransform* wavelet =
      opts.transform == Transform::Laplacian ? nullptr : xf.get();
  Decomposition pyr;
  {
    PRMD_TIMED("decompose");
    pyr = decompose(src, opts.levels, wavelet, opts.downsampler);
  }
  std::vector<Planes> stored;
  {
    PRMD_TIMED("stored_planes");
    stored = stored_planes(pyr, geom, xf.get(), opts, channels);
  }

  const auto coder = make_block_coder(opts.entropy, opts.entropy_level);
  EncodedBlocks blocks;
  {
    PRMD_TIMED("encode_blocks");
    blocks = encode_blocks(stored, geom, xf.get(), *coder, total_blocks,
                           opts.cblk, channels);
  }

  PrmdHeader header = make_header(opts, src, total_blocks);
  std::vector<uint8_t> head;
  {
    PRMD_TIMED("serialize");
    head = serialize(header, level_table(geom), blocks.offsets);
  }
  {
    PRMD_TIMED("write_file");
    write_file(path, head, blocks.data);
  }

  EncodeReport report;
  report.level_bytes = blocks.level_bytes;
  report.file_bytes = head.size() + blocks.data.size();
  report.encode_ms = elapsed_ms(start);
  return report;
}

}  // namespace prmd::internal
