#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/planes.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/internal/transform.h"

namespace prmd::internal {

namespace {

std::vector<int32_t> extract(const Region& plane, const Rect& r) {
  std::vector<int32_t> out(size_t(r.w) * size_t(r.h));
  for (int y = 0; y < r.h; ++y) {
    const int32_t* src = plane.row(r.y + y) + (r.x - plane.rect.x);
    std::memcpy(out.data() + size_t(y) * size_t(r.w), src, size_t(r.w) * sizeof(int32_t));
  }
  return out;
}

}  // namespace

void validate_prmd_options(const EncodeOptions& o) {
  if (o.levels < 1 || o.levels > kMaxLevels) throw std::invalid_argument("levels must be in 1..16");
  if (o.cblk < 16 || o.cblk % 2 || o.cblk > 4096) throw std::invalid_argument("cblk must be even, 16..4096");
  if (o.tile < o.cblk || o.tile % o.cblk || o.tile > 32768)
    throw std::invalid_argument("tile must be a multiple of cblk, <= 32768");
  if (o.quant < 0 || o.quant > 4096) throw std::invalid_argument("quant must be 0 (lossless) .. 4096");
  if (o.quant > 0 && o.transform == Transform::Independent)
    throw std::invalid_argument("lossy (quant > 0) needs transform haar, cdf53 or laplacian; naive is lossless only");
  if (uint8_t(o.transform) > uint8_t(Transform::Independent)) throw std::invalid_argument("bad transform");
  if (uint8_t(o.downsampler) > uint8_t(Downsampler::Gauss)) throw std::invalid_argument("bad downsampler");
  (void)make_block_coder(o.entropy, o.entropy_level);  // validates entropy + level
  (void)info(o.color);
}

EncodeReport encode_prmd(const Image& src_in, const std::string& path, const EncodeOptions& opts) {
  const auto t0 = std::chrono::steady_clock::now();
  validate_prmd_options(opts);
  if (src_in.width <= 0 || src_in.height <= 0) throw std::invalid_argument("empty source image");
  const Image src = convert(src_in, opts.color);
  const int C = src.channels();
  const int N = opts.levels;

  std::unique_ptr<LevelTransform> xf;
  if (opts.transform != Transform::Independent) xf = make_transform(opts.transform, opts.downsampler);

  // 1. Level planes [L][c] and detail bands [L][c][band] of the exact pyramid.
  std::vector<std::vector<Region>> planes(static_cast<size_t>(N));
  std::vector<std::vector<std::vector<Region>>> bands(static_cast<size_t>(N));
  planes[0] = to_planes(src);
  for (int L = 0; L + 1 < N; ++L) {
    bands[size_t(L)].resize(size_t(C));
    for (int c = 0; c < C; ++c) {
      Region coarse;
      const Region& fine = planes[size_t(L)][size_t(c)];
      if (xf)
        xf->forward(fine, coarse, bands[size_t(L)][size_t(c)]);
      else
        coarse = opts.downsampler == Downsampler::Gauss ? downsample_gauss(fine) : downsample_box(fine);
      planes[size_t(L + 1)].push_back(std::move(coarse));
    }
  }

  // 2. What is actually stored, as whole planes: stored[L][band*C + c].
  //    Base levels: pixels (YCoCg-R for RGB); MED is applied per block below.
  //    Detail levels: bands → YCoCg-R (RGB) → quantize. Both steps are per sample, so doing
  //    them on whole planes is identical to doing them per block.
  //    Laplacian is closed-loop: residuals are taken against the *reconstructed* coarser level
  //    (exactly what the decoder will have), so quantization errors never accumulate.
  const auto geom_tmp = make_geometry(src.width, src.height, N, opts.tile, opts.cblk, opts.transform);
  std::vector<std::vector<Region>> stored(static_cast<size_t>(N));
  auto color_fwd = [&](std::vector<Region>& p, size_t first) {
    if (C == 3) ycocg_r_forward(p[first].v.data(), p[first + 1].v.data(), p[first + 2].v.data(), p[first].v.size());
  };
  auto color_inv = [&](std::vector<Region>& p, size_t first) {
    if (C == 3) ycocg_r_inverse(p[first].v.data(), p[first + 1].v.data(), p[first + 2].v.data(), p[first].v.size());
  };
  auto quantize_planes = [&](std::vector<Region>& p, size_t first, int step) {
    for (int c = 0; c < C; ++c)
      for (auto& v : p[first + size_t(c)].v) v = quantize(v, step);
  };
  auto dequantize_planes = [&](std::vector<Region>& p, size_t first, int step) {
    for (int c = 0; c < C; ++c)
      for (auto& v : p[first + size_t(c)].v) v = dequantize(v, step);
  };
  for (int L = N - 1; L >= 0; --L) {
    if (geom_tmp[size_t(L)].base) {
      stored[size_t(L)] = planes[size_t(L)];
      color_fwd(stored[size_t(L)], 0);
    }
  }
  if (xf && opts.transform != Transform::Laplacian) {
    for (int L = 0; L + 1 < N; ++L)
      for (int b = 0; b < xf->num_bands(); ++b) {
        for (int c = 0; c < C; ++c) stored[size_t(L)].push_back(bands[size_t(L)][size_t(c)][size_t(b)]);
        const size_t first = size_t(b * C);
        color_fwd(stored[size_t(L)], first);
        quantize_planes(stored[size_t(L)], first, band_step(xf->band_kind(b), opts.quant));
      }
  } else if (xf) {  // Laplacian, closed loop from the coarsest level down
    std::vector<Region> recon = planes[size_t(N - 1)];  // base is lossless
    for (int L = N - 2; L >= 0; --L) {
      const Rect full{0, 0, geom_tmp[size_t(L)].w, geom_tmp[size_t(L)].h};
      const Dims fd{full.w, full.h};
      std::vector<Region> pred(static_cast<size_t>(C)), res(static_cast<size_t>(C));
      for (int c = 0; c < C; ++c) {
        const std::vector<Region> zero{Region(full)};
        xf->inverse(recon[size_t(c)], zero, full, fd, pred[size_t(c)]);  // U(recon)
        res[size_t(c)] = Region(full);
        for (size_t i = 0; i < res[size_t(c)].v.size(); ++i)
          res[size_t(c)].v[i] = planes[size_t(L)][size_t(c)].v[i] - pred[size_t(c)].v[i];
      }
      color_fwd(res, 0);
      const int step = band_step(Band::Residual, opts.quant);
      quantize_planes(res, 0, step);
      stored[size_t(L)] = res;
      // Reconstruct exactly as the decoder will: dequantize → inverse color → U(recon) + residual.
      dequantize_planes(res, 0, step);
      color_inv(res, 0);
      std::vector<Region> next(static_cast<size_t>(C));
      for (int c = 0; c < C; ++c) {
        const std::vector<Region> band{res[size_t(c)]};
        xf->inverse(recon[size_t(c)], band, full, fd, next[size_t(c)]);
      }
      recon = std::move(next);
    }
  }

  const auto& geom = geom_tmp;
  uint32_t total = 0;
  for (const auto& g : geom) total += g.num_ids;
  const auto coder = make_block_coder(opts.entropy, opts.entropy_level);

  std::vector<uint32_t> offsets(size_t(total) + 1, 0);
  std::vector<uint8_t> data;
  EncodeReport rep;
  for (int L = N - 1; L >= 0; --L) {
    const LevelGeom& g = geom[size_t(L)];
    const size_t level_start = data.size();
    const int k2 = g.k * g.k;
    for (uint32_t local = 0; local < g.num_ids; ++local) {
      const int t = int(local) / k2, in_tile = int(local) % k2;
      const int bx = (t % g.tiles_x) * g.k + in_tile % g.k;
      const int by = (t / g.tiles_x) * g.k + in_tile / g.k;
      const uint32_t id = g.first_block + local;
      if (bx < g.blocks_x && by < g.blocks_y) {
        const Rect R = block_pixel_rect(g, opts.cblk, bx, by);
        BlockPlanes bp;
        bp.channels = C;
        const auto& sp = stored[size_t(L)];
        if (g.base) {
          bp.w = R.w;
          bp.h = R.h;
          for (int c = 0; c < C; ++c) {
            const std::vector<int32_t> px = extract(sp[size_t(c)], R);
            std::vector<int32_t> res(px.size());
            med_residual(px.data(), R.w, R.h, res.data());
            bp.planes.push_back(std::move(res));
            bp.kinds.push_back(Band::Base);
          }
        } else {
          const Rect Rb = xf->band_rect_of_block(R, Dims{g.w, g.h});
          bp.w = Rb.w;
          bp.h = Rb.h;
          for (int b = 0; b < xf->num_bands(); ++b)
            for (int c = 0; c < C; ++c) {
              bp.planes.push_back(extract(sp[size_t(b * C + c)], Rb));
              bp.kinds.push_back(xf->band_kind(b));
            }
        }
        coder->encode(bp, data);
        if (data.size() > 0xFFFFFFFFull) throw std::runtime_error("encoded data exceeds 4 GiB");
      }
      offsets[size_t(id) + 1] = uint32_t(data.size());
    }
    rep.level_bytes[size_t(L)] = data.size() - level_start;
  }

  PrmdHeader h;
  h.transform = opts.transform;
  h.downsampler = opts.downsampler;
  h.width = uint32_t(src.width);
  h.height = uint32_t(src.height);
  h.levels = uint8_t(N);
  h.color = opts.color;
  h.tile = uint16_t(opts.tile);
  h.cblk = uint16_t(opts.cblk);
  h.quant = uint16_t(opts.quant);
  h.total_blocks = total;
  h.entropy = opts.entropy;
  h.entropy_level = uint8_t(opts.entropy_level);
  std::vector<LevelEntry> table;
  for (const auto& g : geom)
    table.push_back(LevelEntry{uint32_t(g.w), uint32_t(g.h), uint16_t(g.blocks_x), uint16_t(g.blocks_y), g.first_block});
  const auto head = serialize(h, table, offsets);

  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) throw IoError("cannot create '" + path + "'");
  const bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
                  std::fwrite(data.data(), 1, data.size(), f) == data.size();
  const bool closed = std::fclose(f) == 0;
  if (!ok || !closed) throw IoError("write failed for '" + path + "'");

  rep.file_bytes = head.size() + data.size();
  rep.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return rep;
}

}  // namespace prmd::internal
