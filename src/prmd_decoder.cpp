// .prmd decoder: recursive region reconstruction (design.md §6).

#include <algorithm>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/file.h"
#include "prmd/internal/planes.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/internal/transform.h"
#include "prmd/profile.h"

namespace prmd::internal {

namespace {

// Decoded samples of one code-block: one Region per (band, channel),
// band-major. For Base blocks the Regions are pixel values (MED and YCoCg-R,
// where the file uses them, already undone).
struct DecodedBlock {
  std::vector<Region> planes;

  size_t bytes() const {
    size_t n = 0;
    for (const auto& p : planes) {
      n += p.v.size() * sizeof(int32_t);
    }
    return n + sizeof(DecodedBlock);
  }
};
using BlockPtr = std::shared_ptr<const DecodedBlock>;

// Distinct (level, tile index) pairs touched by one decode.
using TileSet = std::set<std::pair<int, int>>;

class LruCache {
 public:
  explicit LruCache(size_t budget) : budget_(budget) {}

  bool enabled() const { return budget_ > 0; }

  BlockPtr get(uint32_t id) {
    auto it = map_.find(id);
    if (it == map_.end()) {
      return nullptr;
    }
    order_.splice(order_.begin(), order_, it->second);
    return it->second->second;
  }

  void put(uint32_t id, BlockPtr b) {
    if (!enabled()) {
      return;
    }
    const size_t size = b->bytes();
    if (size > budget_) {
      return;
    }

    order_.emplace_front(id, std::move(b));
    map_[id] = order_.begin();
    used_ += size;

    while (used_ > budget_) {
      auto& last = order_.back();
      used_ -= last.second->bytes();
      map_.erase(last.first);
      order_.pop_back();
    }
  }

 private:
  using Entry = std::pair<uint32_t, BlockPtr>;

  size_t budget_;
  size_t used_ = 0;
  std::list<Entry> order_;  // most recently used first
  std::unordered_map<uint32_t, std::list<Entry>::iterator> map_;
};

class PrmdDecoder final : public CropDecoder {
 public:
  PrmdDecoder(const std::string& path, const DecoderOptions& opts)
      : file_(path), cache_(opts.cache_bytes) {
    PRMD_TIMED("open_prmd");
    file_bytes_ = file_.size();
    read_header_and_index();

    geom_ = make_geometry(int(h_.width), int(h_.height), h_.levels, h_.tile,
                          h_.cblk, h_.transform);
    if (h_.transform != Transform::Independent) {
      xf_ = make_transform(h_.transform, h_.downsampler);
    }
    coder_ = make_block_coder(h_.entropy, 0);
    channels_ = channels(h_.color);
  }

  std::string name() const override {
    std::string n = std::string("prmd/") + to_string(h_.transform);
    if (h_.transform == Transform::Laplacian ||
        h_.transform == Transform::Independent) {
      n += std::string("-") + to_string(h_.downsampler);
    }
    if (h_.entropy != Entropy::Zstd) {
      n += std::string("+") + to_string(h_.entropy);
    }
    if (h_.quant > 0) {
      n += "/q" + std::to_string(h_.quant);
    }
    if (!h_.med) {
      n += "/no-med";
    }
    if (!h_.ycocg) {
      n += "/no-ycocg";
    }
    return n;
  }

  int num_levels() const override { return h_.levels; }
  ColorFormat color_format() const override { return h_.color; }

  LevelInfo level_info(int level) const override {
    if (level < 0 || level >= num_levels()) {
      throw std::out_of_range("level out of range");
    }
    return LevelInfo{geom_[size_t(level)].w, geom_[size_t(level)].h};
  }

  uint64_t file_bytes() const override { return file_bytes_; }
  uint64_t open_bytes_read() const override { return open_bytes_; }

 protected:
  Image decode_region(int level, const Rect& r, DecodeStats& stats) override {
    TileSet tiles;
    const auto planes = reconstruct(level, r, stats, tiles);
    stats.tiles_touched = int(tiles.size());
    PRMD_TIMED("from_planes");
    return from_planes(planes, h_.color);
  }

 private:
  // A block that is not in the cache.
  struct Miss {
    uint32_t id;
    int bx;
    int by;
    bool operator<(const Miss& o) const {
      return std::tie(id, bx, by) < std::tie(o.id, o.bx, o.by);
    }
  };

  void read_header_and_index() {
    PRMD_TIMED("read_header_index");
    if (file_bytes_ < kHeaderSize) {
      throw FormatError("file too small for a .prmd header");
    }
    uint8_t head[kHeaderSize];
    file_.read_at(0, head, kHeaderSize);
    h_ = parse_header(head, kHeaderSize);

    // data_offset == header + index size (validated by parse_header), so
    // checking it against the file size first bounds the allocation below.
    if (h_.data_offset > file_bytes_) {
      throw FormatError("truncated index");
    }
    std::vector<uint8_t> index(index_bytes(h_));
    file_.read_at(kHeaderSize, index.data(), index.size());

    std::vector<LevelEntry> table;
    parse_index(h_, index.data(), index.size(), table, offsets_);
    if (h_.data_offset + offsets_.back() != file_bytes_) {
      throw FormatError("file size does not match index");
    }
    open_bytes_ = kHeaderSize + index.size();
  }

  // Block-grid size in band (or pixel) samples for `level`.
  int block_span(int level) const {
    const LevelGeom& g = geom_[size_t(level)];
    if (g.base || xf_->num_bands() == 1) {
      return h_.cblk;
    }
    return h_.cblk / 2;
  }

  // Region of `level`'s pixels covering exactly r, one Region per channel.
  std::vector<Region> reconstruct(int level,
                                  const Rect& r,
                                  DecodeStats& stats,
                                  TileSet& tiles) {
    const LevelGeom& g = geom_[size_t(level)];
    const Dims fd{g.w, g.h};

    if (g.base) {
      const auto blocks = fetch(level, r, stats, tiles);
      PRMD_TIMED("copy_blocks");
      std::vector<Region> out(static_cast<size_t>(channels_), Region(r));
      for (const auto& b : blocks) {
        for (int c = 0; c < channels_; ++c) {
          copy_overlap(b->planes[size_t(c)], out[size_t(c)]);
        }
      }
      return out;
    }

    const Rect coarse_rect = xf_->coarse_needed(r, fd);
    const Rect band_rect = xf_->band_needed(r, fd);
    const std::vector<Region> coarse =
        reconstruct(level + 1, coarse_rect, stats, tiles);
    const auto blocks = fetch(level, band_rect, stats, tiles);

    const int nb = xf_->num_bands();
    std::vector<Region> out(static_cast<size_t>(channels_));
    std::vector<Region> bands(static_cast<size_t>(nb), Region(band_rect));
    for (int c = 0; c < channels_; ++c) {
      {
        PRMD_TIMED("copy_blocks");
        for (int b = 0; b < nb; ++b) {
          for (const auto& blk : blocks) {
            copy_overlap(blk->planes[size_t(b * channels_ + c)],
                         bands[size_t(b)]);
          }
        }
      }
      PRMD_TIMED("inverse");
      xf_->inverse(coarse[size_t(c)], bands, r, fd, out[size_t(c)]);
    }
    return out;
  }

  // Blocks of `level` whose stored samples overlap `need` (pixels for base
  // levels, band samples for detail levels).
  std::vector<BlockPtr> fetch(int level,
                              const Rect& need,
                              DecodeStats& stats,
                              TileSet& tiles) {
    const LevelGeom& g = geom_[size_t(level)];
    const int span = block_span(level);
    const int bx0 = need.x / span;
    const int by0 = need.y / span;
    const int bx1 = std::min((need.right() - 1) / span, g.blocks_x - 1);
    const int by1 = std::min((need.bottom() - 1) / span, g.blocks_y - 1);

    std::vector<BlockPtr> out;
    std::vector<Miss> misses;
    {
      PRMD_TIMED("block_lookup");
      for (int by = by0; by <= by1; ++by) {
        for (int bx = bx0; bx <= bx1; ++bx) {
          tiles.insert({level, tile_index(g, bx, by)});
          const uint32_t id = block_id(g, bx, by);
          if (BlockPtr b = cache_.enabled() ? cache_.get(id) : nullptr) {
            ++stats.cache_hits;
            out.push_back(std::move(b));
          } else {
            misses.push_back({id, bx, by});
          }
        }
      }

      // Coalesce runs of consecutive ids (contiguous on disk) into one read.
      std::sort(misses.begin(), misses.end());
    }
    for (size_t i = 0; i < misses.size();) {
      size_t j = i + 1;
      while (j < misses.size() && misses[j].id == misses[j - 1].id + 1) {
        ++j;
      }
      read_run(level, misses.data() + i, misses.data() + j, stats, out);
      i = j;
    }
    return out;
  }

  // Reads the payloads of [first, last) — consecutive block ids — with one
  // read, decodes each block and appends it to `out`.
  void read_run(int level,
                const Miss* first,
                const Miss* last,
                DecodeStats& stats,
                std::vector<BlockPtr>& out) {
    const uint64_t begin = offsets_[first->id];
    const uint64_t end = offsets_[size_t((last - 1)->id) + 1];
    std::vector<uint8_t> buf(size_t(end - begin));
    if (!buf.empty()) {
      PRMD_TIMED("pread");
      stats.read_calls +=
          file_.read_at(h_.data_offset + begin, buf.data(), buf.size());
    }
    stats.bytes_read += buf.size();

    for (const Miss* m = first; m != last; ++m) {
      const uint32_t id = m->id;
      const uint8_t* payload = buf.data() + (offsets_[id] - begin);
      const size_t size = offsets_[size_t(id) + 1] - offsets_[id];
      BlockPtr blk = decode_block(level, m->bx, m->by, payload, size);

      ++stats.blocks_decoded;
      ++stats.blocks_per_level[size_t(level)];
      if (cache_.enabled()) {
        PRMD_TIMED("cache_put");
        cache_.put(id, blk);
      }
      out.push_back(std::move(blk));
    }
  }

  BlockPtr decode_block(int level,
                        int bx,
                        int by,
                        const uint8_t* data,
                        size_t size) const {
    PRMD_TIMED("decode_block");
    const LevelGeom& g = geom_[size_t(level)];
    const Rect pixels = block_pixel_rect(g, h_.cblk, bx, by);
    const Rect samples =
        g.base ? pixels : xf_->band_rect_of_block(pixels, Dims{g.w, g.h});
    const int nb = g.base ? 1 : xf_->num_bands();

    BlockPlanes bp;
    bp.w = samples.w;
    bp.h = samples.h;
    bp.channels = channels_;
    for (int b = 0; b < nb; ++b) {
      for (int c = 0; c < channels_; ++c) {
        bp.kinds.push_back(g.base ? Band::Base : xf_->band_kind(b));
      }
    }
    {
      PRMD_TIMED("entropy_decode");
      coder_->decode(data, size, bp);
    }

    for (int b = 0; b < nb; ++b) {
      undo_block_coding(g.base, b, samples, bp);
    }

    PRMD_TIMED("wrap_block");
    auto blk = std::make_shared<DecodedBlock>();
    for (auto& p : bp.planes) {
      Region reg;
      reg.rect = samples;
      reg.v = std::move(p);
      blk->planes.push_back(std::move(reg));
    }
    return blk;
  }

  // Inverts the per-sample steps applied after the transform for band `b`
  // (planes b*C .. b*C+C-1): dequantization (detail) or MED (base), then
  // YCoCg-R. MED and YCoCg-R are skipped when the header turns them off.
  void undo_block_coding(bool base,
                         int b,
                         const Rect& samples,
                         BlockPlanes& bp) const {
    const size_t first = size_t(b * channels_);
    const size_t n = size_t(samples.w) * size_t(samples.h);

    if (base) {
      if (h_.med) {
        PRMD_TIMED("med_reconstruct");
        for (int c = 0; c < channels_; ++c) {
          std::vector<int32_t> px(n);
          med_reconstruct(bp.planes[first + size_t(c)].data(), samples.w,
                          samples.h, px.data());
          bp.planes[first + size_t(c)].swap(px);
        }
      }
    } else {
      const int step = band_step(xf_->band_kind(b), h_.quant);
      if (step > 1) {
        PRMD_TIMED("dequantize");
        for (int c = 0; c < channels_; ++c) {
          for (auto& v : bp.planes[first + size_t(c)]) {
            v = dequantize(v, step);
          }
        }
      }
    }

    if (h_.ycocg && channels_ == 3) {
      PRMD_TIMED("ycocg_inverse");
      ycocg_r_inverse(bp.planes[first].data(), bp.planes[first + 1].data(),
                      bp.planes[first + 2].data(), n);
    }
  }

  ReadOnlyFile file_;
  LruCache cache_;
  PrmdHeader h_;
  std::vector<uint32_t> offsets_;
  std::vector<LevelGeom> geom_;
  std::unique_ptr<LevelTransform> xf_;
  std::unique_ptr<BlockCoder> coder_;
  int channels_ = 1;
  uint64_t file_bytes_ = 0;
  uint64_t open_bytes_ = 0;
};

}  // namespace

std::unique_ptr<CropDecoder> open_prmd(const std::string& path,
                                       const DecoderOptions& opts) {
  return std::make_unique<PrmdDecoder>(path, opts);
}

}  // namespace prmd::internal
