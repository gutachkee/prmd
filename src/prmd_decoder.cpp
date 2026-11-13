// .prmd decoder: recursive region reconstruction (DESIGN.md §6).

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/planes.h"
#include "prmd/internal/prmd_codec.h"
#include "prmd/internal/prmd_format.h"
#include "prmd/internal/transform.h"

namespace prmd::internal {

namespace {

// Decoded samples of one code-block: one Region per (band, channel), band-major.
// For Base blocks the Regions are pixel values (MED and YCoCg-R already undone).
struct DecodedBlock {
  std::vector<Region> planes;
  size_t bytes() const {
    size_t n = 0;
    for (const auto& p : planes) n += p.v.size() * sizeof(int32_t);
    return n + sizeof(DecodedBlock);
  }
};
using BlockPtr = std::shared_ptr<const DecodedBlock>;

class LruCache {
 public:
  explicit LruCache(size_t budget) : budget_(budget) {}
  bool enabled() const { return budget_ > 0; }
  BlockPtr get(uint32_t id) {
    auto it = map_.find(id);
    if (it == map_.end()) return nullptr;
    order_.splice(order_.begin(), order_, it->second);
    return it->second->second;
  }
  void put(uint32_t id, BlockPtr b) {
    if (!enabled()) return;
    const size_t sz = b->bytes();
    if (sz > budget_) return;
    order_.emplace_front(id, std::move(b));
    map_[id] = order_.begin();
    used_ += sz;
    while (used_ > budget_) {
      auto& last = order_.back();
      used_ -= last.second->bytes();
      map_.erase(last.first);
      order_.pop_back();
    }
  }

 private:
  size_t budget_, used_ = 0;
  std::list<std::pair<uint32_t, BlockPtr>> order_;
  std::unordered_map<uint32_t, std::list<std::pair<uint32_t, BlockPtr>>::iterator> map_;
};

class Fd {
 public:
  explicit Fd(const std::string& path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
    if (fd_ < 0) throw IoError("cannot open '" + path + "': " + std::strerror(errno));
  }
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  uint64_t size() const {
    struct stat st;
    if (::fstat(fd_, &st) != 0) throw IoError("fstat failed");
    return uint64_t(st.st_size);
  }
  // Reads exactly n bytes at offset (looping over short reads); returns the number of
  // pread() calls issued.
  int read_at(uint64_t off, uint8_t* dst, size_t n) const {
    int calls = 0;
    while (n > 0) {
      const ssize_t r = ::pread(fd_, dst, n, off_t(off));
      ++calls;
      if (r < 0) {
        if (errno == EINTR) continue;
        throw IoError(std::string("pread failed: ") + std::strerror(errno));
      }
      if (r == 0) throw FormatError("unexpected end of file");
      dst += r;
      off += uint64_t(r);
      n -= size_t(r);
    }
    return calls;
  }

 private:
  int fd_;
};

class PrmdDecoder final : public CropDecoder {
 public:
  PrmdDecoder(const std::string& path, const DecoderOptions& opts) : fd_(path), cache_(opts.cache_bytes) {
    file_bytes_ = fd_.size();
    uint8_t head[kHeaderSize];
    if (file_bytes_ < kHeaderSize) throw FormatError("file too small for a .prmd header");
    fd_.read_at(0, head, kHeaderSize);
    h_ = parse_header(head, kHeaderSize);
    std::vector<uint8_t> idx(index_bytes(h_));
    if (h_.data_offset > file_bytes_) throw FormatError("truncated index");
    fd_.read_at(kHeaderSize, idx.data(), idx.size());
    std::vector<LevelEntry> table;
    parse_index(h_, idx.data(), idx.size(), table, offsets_);
    if (h_.data_offset + offsets_.back() != file_bytes_) throw FormatError("file size does not match index");
    open_bytes_ = kHeaderSize + idx.size();
    geom_ = make_geometry(int(h_.width), int(h_.height), h_.levels, h_.tile, h_.cblk, h_.transform);
    if (h_.transform != Transform::Independent) xf_ = make_transform(h_.transform, h_.downsampler);
    coder_ = make_block_coder(h_.entropy, 0);
    channels_ = channels(h_.color);
  }

  std::string name() const override {
    std::string n = std::string("prmd/") + to_string(h_.transform);
    if (h_.transform == Transform::Laplacian || h_.transform == Transform::Independent)
      n += std::string("-") + to_string(h_.downsampler);
    if (h_.entropy != Entropy::Zstd) n += std::string("+") + to_string(h_.entropy);
    if (h_.quant > 0) n += "/q" + std::to_string(h_.quant);
    return n;
  }
  int num_levels() const override { return h_.levels; }
  ColorFormat color_format() const override { return h_.color; }
  LevelInfo level_info(int level) const override {
    if (level < 0 || level >= num_levels()) throw std::out_of_range("level out of range");
    return LevelInfo{geom_[size_t(level)].w, geom_[size_t(level)].h};
  }
  uint64_t file_bytes() const override { return file_bytes_; }
  uint64_t open_bytes_read() const override { return open_bytes_; }

 protected:
  Image decode_region(int level, const Rect& r, DecodeStats& stats) override {
    std::set<std::pair<int, int>> tiles;
    auto planes = reconstruct(level, r, stats, tiles);
    stats.tiles_touched = int(tiles.size());
    return from_planes(planes, h_.color);
  }

 private:
  // Block-grid size in band (or pixel) samples for level L.
  int block_span(int L) const {
    const auto& g = geom_[size_t(L)];
    if (g.base || xf_->num_bands() == 1) return h_.cblk;
    return h_.cblk / 2;
  }

  // Region of level L's pixels covering exactly R, one Region per channel.
  std::vector<Region> reconstruct(int L, const Rect& R, DecodeStats& st, std::set<std::pair<int, int>>& tiles) {
    const LevelGeom& g = geom_[size_t(L)];
    const Dims fd{g.w, g.h};
    if (g.base) {
      auto blocks = fetch(L, R, st, tiles);
      std::vector<Region> out(static_cast<size_t>(channels_), Region(R));
      for (const auto& b : blocks)
        for (int c = 0; c < channels_; ++c) copy_overlap(b->planes[size_t(c)], out[size_t(c)]);
      return out;
    }
    const Rect Rc = xf_->coarse_needed(R, fd);
    const Rect Rb = xf_->band_needed(R, fd);
    const std::vector<Region> coarse = reconstruct(L + 1, Rc, st, tiles);
    auto blocks = fetch(L, Rb, st, tiles);
    const int nb = xf_->num_bands();
    std::vector<Region> out(static_cast<size_t>(channels_));
    std::vector<Region> bands(static_cast<size_t>(nb), Region(Rb));
    for (int c = 0; c < channels_; ++c) {
      for (int b = 0; b < nb; ++b) {
        for (const auto& blk : blocks) copy_overlap(blk->planes[size_t(b * channels_ + c)], bands[size_t(b)]);
      }
      xf_->inverse(coarse[size_t(c)], bands, R, fd, out[size_t(c)]);
    }
    return out;
  }

  // Blocks of level L whose stored samples overlap `need` (pixels for base levels,
  // band samples for detail levels).
  std::vector<BlockPtr> fetch(int L, const Rect& need, DecodeStats& st, std::set<std::pair<int, int>>& tiles) {
    const LevelGeom& g = geom_[size_t(L)];
    const int span = block_span(L);
    const int bx0 = need.x / span, bx1 = std::min((need.right() - 1) / span, g.blocks_x - 1);
    const int by0 = need.y / span, by1 = std::min((need.bottom() - 1) / span, g.blocks_y - 1);
    std::vector<BlockPtr> out;
    std::vector<std::pair<uint32_t, std::pair<int, int>>> miss;
    for (int by = by0; by <= by1; ++by)
      for (int bx = bx0; bx <= bx1; ++bx) {
        tiles.insert({L, tile_index(g, bx, by)});
        const uint32_t id = block_id(g, bx, by);
        if (BlockPtr b = cache_.enabled() ? cache_.get(id) : nullptr) {
          ++st.cache_hits;
          out.push_back(std::move(b));
        } else {
          miss.push_back({id, {bx, by}});
        }
      }
    std::sort(miss.begin(), miss.end());
    // Coalesce runs of consecutive ids (contiguous on disk) into one read each.
    for (size_t i = 0; i < miss.size();) {
      size_t j = i + 1;
      while (j < miss.size() && miss[j].first == miss[j - 1].first + 1) ++j;
      const uint32_t first = miss[i].first, last = miss[j - 1].first;
      const uint64_t off = offsets_[first], end = offsets_[size_t(last) + 1];
      std::vector<uint8_t> buf(size_t(end - off));
      if (!buf.empty()) st.read_calls += fd_.read_at(h_.data_offset + off, buf.data(), buf.size());
      st.bytes_read += buf.size();
      for (size_t m = i; m < j; ++m) {
        const uint32_t id = miss[m].first;
        const auto [bx, by] = miss[m].second;
        auto blk = decode_block(L, bx, by, buf.data() + (offsets_[id] - off), offsets_[size_t(id) + 1] - offsets_[id]);
        ++st.blocks_decoded;
        ++st.blocks_per_level[size_t(L)];
        cache_.put(id, blk);
        out.push_back(std::move(blk));
      }
      i = j;
    }
    return out;
  }

  BlockPtr decode_block(int L, int bx, int by, const uint8_t* data, size_t size) const {
    const LevelGeom& g = geom_[size_t(L)];
    const Rect R = block_pixel_rect(g, h_.cblk, bx, by);
    const Rect S = g.base ? R : xf_->band_rect_of_block(R, Dims{g.w, g.h});
    BlockPlanes bp;
    bp.w = S.w;
    bp.h = S.h;
    bp.channels = channels_;
    const int nb = g.base ? 1 : xf_->num_bands();
    for (int b = 0; b < nb; ++b)
      for (int c = 0; c < channels_; ++c) bp.kinds.push_back(g.base ? Band::Base : xf_->band_kind(b));
    coder_->decode(data, size, bp);
    auto blk = std::make_shared<DecodedBlock>();
    const size_t n = size_t(S.w) * size_t(S.h);
    for (int b = 0; b < nb; ++b) {
      const size_t first = size_t(b * channels_);
      if (!g.base) {
        const int step = band_step(xf_->band_kind(b), h_.quant);
        if (step > 1)
          for (int c = 0; c < channels_; ++c)
            for (auto& v : bp.planes[first + size_t(c)]) v = dequantize(v, step);
      }
      if (g.base)
        for (int c = 0; c < channels_; ++c) {
          std::vector<int32_t> px(n);
          med_reconstruct(bp.planes[first + size_t(c)].data(), S.w, S.h, px.data());
          bp.planes[first + size_t(c)].swap(px);
        }
      if (channels_ == 3)
        ycocg_r_inverse(bp.planes[first].data(), bp.planes[first + 1].data(), bp.planes[first + 2].data(), n);
    }
    for (auto& p : bp.planes) {
      Region reg;
      reg.rect = S;
      reg.v = std::move(p);
      blk->planes.push_back(std::move(reg));
    }
    return blk;
  }

  Fd fd_;
  LruCache cache_;
  PrmdHeader h_;
  std::vector<uint32_t> offsets_;
  std::vector<LevelGeom> geom_;
  std::unique_ptr<LevelTransform> xf_;
  std::unique_ptr<BlockCoder> coder_;
  int channels_ = 1;
  uint64_t file_bytes_ = 0, open_bytes_ = 0;
};

}  // namespace

std::unique_ptr<CropDecoder> open_prmd(const std::string& path, const DecoderOptions& opts) {
  return std::make_unique<PrmdDecoder>(path, opts);
}

}  // namespace prmd::internal
