// JPEG2000 backend (OpenJPEG 2.5).
//
// Encoder: tiled codestream (.j2k), reversible 5/3 (lossless) or 9/7 (lossy,
// PSNR target), numresolution = levels so that reduce = L yields level L, TLM
// tile-length index.
//
// Decoder: OpenJPEG decodes only once per codec instance, so every crop
// creates a fresh codec. The main header is read from the file once at open()
// and served from RAM to each per-crop codec (its parse cost stays in the crop
// latency; its bytes are not re-read). Tile data comes from the file through a
// counting stream (pread), so DecodeStats reports real bytes and read calls,
// including OpenJPEG's own read-ahead (stream buffer 4 KiB).

#include "prmd/internal/j2k_codec.h"

#include <sys/stat.h>
#include <unistd.h>

#include <openjpeg.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "prmd/errors.h"
#include "prmd/internal/file.h"
#include "prmd/internal/parallel.h"
#include "prmd/internal/timing.h"

namespace prmd::internal {

namespace {

constexpr OPJ_SIZE_T kStreamBuffer = 4096;

// ----------------------------------------------------------------------------
// OpenJPEG handles and callbacks

struct Messages {
  std::string last_error;
};

void on_error(const char* msg, void* user) {
  auto* m = static_cast<Messages*>(user);
  m->last_error = msg;
  while (!m->last_error.empty() && m->last_error.back() == '\n') {
    m->last_error.pop_back();
  }
}

void on_quiet(const char*, void*) {}

struct CodecDeleter {
  void operator()(opj_codec_t* c) const {
    if (c) {
      opj_destroy_codec(c);
    }
  }
};

struct StreamDeleter {
  void operator()(opj_stream_t* s) const {
    if (s) {
      opj_stream_destroy(s);
    }
  }
};

struct ImageDeleter {
  void operator()(opj_image_t* i) const {
    if (i) {
      opj_image_destroy(i);
    }
  }
};

using CodecPtr = std::unique_ptr<opj_codec_t, CodecDeleter>;
using StreamPtr = std::unique_ptr<opj_stream_t, StreamDeleter>;
using ImagePtr = std::unique_ptr<opj_image_t, ImageDeleter>;

// Errors go to `msgs`; warnings and info are dropped.
void set_handlers(opj_codec_t* codec, Messages* msgs) {
  opj_set_error_handler(codec, on_error, msgs);
  opj_set_warning_handler(codec, on_quiet, nullptr);
  opj_set_info_handler(codec, on_quiet, nullptr);
}

// ----------------------------------------------------------------------------
// Read stream

// Read stream over a file: bytes below `cached.size()` come from RAM, the rest
// via pread.
struct ReadSource {
  int fd = -1;
  uint64_t size = 0;
  uint64_t pos = 0;
  const std::vector<uint8_t>* cached = nullptr;
  uint64_t file_bytes_read = 0;
  int file_reads = 0;
};

OPJ_SIZE_T src_read(void* buf, OPJ_SIZE_T n, void* user) {
  auto* s = static_cast<ReadSource*>(user);
  if (s->pos >= s->size) {
    return OPJ_SIZE_T(-1);
  }
  n = OPJ_SIZE_T(std::min<uint64_t>(n, s->size - s->pos));
  auto* out = static_cast<uint8_t*>(buf);
  OPJ_SIZE_T done = 0;

  // Serve the cached prefix from RAM.
  const uint64_t cached = s->cached ? s->cached->size() : 0;
  if (s->pos < cached) {
    const OPJ_SIZE_T k = OPJ_SIZE_T(std::min<uint64_t>(n, cached - s->pos));
    std::memcpy(out, s->cached->data() + s->pos, k);
    done += k;
  }

  // The rest from the file.
  if (done < n) {
    ++s->file_reads;
    while (done < n) {
      const ssize_t r =
          ::pread(s->fd, out + done, n - done, off_t(s->pos + done));
      if (r < 0 && errno == EINTR) {
        continue;
      }
      if (r <= 0) {
        break;
      }
      s->file_bytes_read += uint64_t(r);
      done += OPJ_SIZE_T(r);
    }
  }

  s->pos += done;
  return done == 0 ? OPJ_SIZE_T(-1) : done;
}

OPJ_OFF_T src_skip(OPJ_OFF_T n, void* user) {
  auto* s = static_cast<ReadSource*>(user);
  if (n < 0 && uint64_t(-n) > s->pos) {
    return -1;
  }
  s->pos = uint64_t(int64_t(s->pos) + n);
  return n;
}

OPJ_BOOL src_seek(OPJ_OFF_T n, void* user) {
  auto* s = static_cast<ReadSource*>(user);
  if (n < 0 || uint64_t(n) > s->size) {
    return OPJ_FALSE;
  }
  s->pos = uint64_t(n);
  return OPJ_TRUE;
}

StreamPtr make_read_stream(ReadSource* src) {
  StreamPtr s(opj_stream_create(kStreamBuffer, OPJ_TRUE));
  if (!s) {
    throw std::runtime_error("opj_stream_create failed");
  }
  opj_stream_set_user_data(s.get(), src, nullptr);
  opj_stream_set_user_data_length(s.get(), src->size);
  opj_stream_set_read_function(s.get(), src_read);
  opj_stream_set_skip_function(s.get(), src_skip);
  opj_stream_set_seek_function(s.get(), src_seek);
  return s;
}

CodecPtr make_decoder(Messages* msgs) {
  CodecPtr c(opj_create_decompress(OPJ_CODEC_J2K));
  if (!c) {
    throw std::runtime_error("opj_create_decompress failed");
  }
  set_handlers(c.get(), msgs);

  opj_dparameters_t p;
  opj_set_default_decoder_parameters(&p);
  if (!opj_setup_decoder(c.get(), &p)) {
    throw std::runtime_error("opj_setup_decoder failed");
  }
  return c;
}

ImagePtr read_header(opj_stream_t* stream,
                     opj_codec_t* codec,
                     const Messages& msgs) {
  opj_image_t* raw = nullptr;
  if (!opj_read_header(stream, codec, &raw)) {
    throw FormatError("J2K header: " + msgs.last_error);
  }
  return ImagePtr(raw);
}

// ----------------------------------------------------------------------------
// Decoder

class J2kDecoder final : public CropDecoder {
 public:
  explicit J2kDecoder(const std::string& path) : file_(path) {
    file_bytes_ = file_.size();

    ReadSource src{file_.fd(), file_bytes_, 0, nullptr, 0, 0};
    Messages msgs;
    CodecPtr codec = make_decoder(&msgs);
    StreamPtr stream = make_read_stream(&src);
    const ImagePtr img = read_header(stream.get(), codec.get(), msgs);
    open_bytes_ = src.file_bytes_read;

    read_codestream_info(codec.get());
    const uint64_t main_end = main_header_length();
    read_image_info(*img);
    read_main_header(main_end);
  }

  std::string name() const override {
    return "j2k-t" + std::to_string(tile_w_);
  }

  int num_levels() const override { return levels_; }
  ColorFormat color_format() const override { return format_; }

  LevelInfo level_info(int level) const override {
    if (level < 0 || level >= levels_) {
      throw std::out_of_range("level out of range");
    }
    return LevelInfo{(width_ + (1 << level) - 1) >> level,
                     (height_ + (1 << level) - 1) >> level};
  }

  uint64_t file_bytes() const override { return file_bytes_; }
  uint64_t open_bytes_read() const override { return open_bytes_; }

 protected:
  Image decode_region(int level, const Rect& r, DecodeStats& stats) override {
    ReadSource src{file_.fd(), file_bytes_, 0, &header_, 0, 0};
    Messages msgs;
    CodecPtr codec = make_decoder(&msgs);
    StreamPtr stream = make_read_stream(&src);
    const ImagePtr img = read_header(stream.get(), codec.get(), msgs);

    // Full-resolution window of the level-L rect.
    const int x0 = r.x << level;
    const int y0 = r.y << level;
    const int x1 = std::min(r.right() << level, width_);
    const int y1 = std::min(r.bottom() << level, height_);
    if (!opj_set_decoded_resolution_factor(codec.get(), OPJ_UINT32(level)) ||
        !opj_set_decode_area(codec.get(), img.get(), x0, y0, x1, y1) ||
        !opj_decode(codec.get(), stream.get(), img.get()) ||
        !opj_end_decompress(codec.get(), stream.get())) {
      throw FormatError("J2K decode: " + msgs.last_error);
    }

    Image out = to_image(*img, r);

    const int tiles_x =
        ((x1 - 1 - tile_x0_) / tile_w_) - ((x0 - tile_x0_) / tile_w_) + 1;
    const int tiles_y =
        ((y1 - 1 - tile_y0_) / tile_h_) - ((y0 - tile_y0_) / tile_h_) + 1;
    stats.tiles_touched = tiles_x * tiles_y;
    stats.blocks_decoded = stats.tiles_touched;
    stats.blocks_per_level[size_t(level)] = stats.tiles_touched;
    stats.bytes_read = src.file_bytes_read;
    stats.read_calls = src.file_reads;
    return out;
  }

 private:
  void read_codestream_info(opj_codec_t* codec) {
    opj_codestream_info_v2_t* info = opj_get_cstr_info(codec);
    if (!info) {
      throw FormatError("J2K: no codestream info");
    }
    levels_ = int(info->m_default_tile_info.tccp_info[0].numresolutions);
    // Foreign files may carry up to 33 resolutions; DecodeStats and the
    // level shifts below assume at most kMaxLevels.
    if (levels_ > kMaxLevels) {
      levels_ = kMaxLevels;
    }
    tile_w_ = int(info->tdx);
    tile_h_ = int(info->tdy);
    tile_x0_ = int(info->tx0);
    tile_y0_ = int(info->ty0);
    opj_destroy_cstr_info(&info);
  }

  void read_image_info(const opj_image_t& img) {
    if (img.x0 != 0 || img.y0 != 0) {
      throw FormatError("J2K: non-zero image origin is not supported");
    }
    width_ = int(img.x1);
    height_ = int(img.y1);

    if (img.numcomps == 1) {
      format_ = ColorFormat::Gray8;
    } else if (img.numcomps == 3) {
      format_ = ColorFormat::RGB8;
    } else {
      throw FormatError("J2K: unsupported component count " +
                        std::to_string(img.numcomps));
    }

    for (OPJ_UINT32 c = 0; c < img.numcomps; ++c) {
      const opj_image_comp_t& comp = img.comps[c];
      if (comp.prec != 8 || comp.sgnd || comp.dx != 1 || comp.dy != 1) {
        throw FormatError(
            "J2K: only unsigned 8-bit, unsubsampled components are "
            "supported");
      }
    }
  }

  // Length of the main header: walk marker segments from SOC (each is
  // 0xFF xx + u16 length) until the first SOT (0xFF90). Reads only the main
  // header, never tile data.
  uint64_t main_header_length() const {
    uint64_t pos = 2;  // after SOC
    for (int guard = 0; guard < 4096; ++guard) {
      uint8_t m[4];
      if (::pread(file_.fd(), m, 4, off_t(pos)) != 4) {
        break;
      }
      if (m[0] != 0xFF) {
        break;
      }
      if (m[1] == 0x90) {
        return pos;  // SOT
      }
      pos += 2 + ((uint64_t(m[2]) << 8) | m[3]);
    }
    throw FormatError("J2K: cannot locate the end of the main header");
  }

  void read_main_header(uint64_t length) {
    header_.resize(size_t(length));
    for (size_t done = 0; done < header_.size();) {
      const ssize_t r = ::pread(file_.fd(), header_.data() + done,
                                header_.size() - done, off_t(done));
      if (r <= 0) {
        throw IoError("J2K: cannot read main header");
      }
      done += size_t(r);
    }
  }

  // Interleaves the decoded components into an Image of r's size.
  Image to_image(const opj_image_t& img, const Rect& r) const {
    Image out(r.w, r.h, format_);
    const int channels = out.channels();

    for (int c = 0; c < channels; ++c) {
      const opj_image_comp_t& comp = img.comps[c];
      if (int(comp.w) != r.w || int(comp.h) != r.h) {
        throw FormatError("J2K decode: unexpected output size " +
                          std::to_string(comp.w) + "x" +
                          std::to_string(comp.h));
      }
      for (int y = 0; y < r.h; ++y) {
        uint8_t* dst = out.row(y);
        const OPJ_INT32* src = comp.data + size_t(y) * comp.w;
        for (int x = 0; x < r.w; ++x) {
          dst[size_t(x) * size_t(channels) + size_t(c)] = clamp_to_u8(src[x]);
        }
      }
    }
    return out;
  }

  ReadOnlyFile file_;
  uint64_t file_bytes_ = 0;
  uint64_t open_bytes_ = 0;
  std::vector<uint8_t> header_;  // main header, served from RAM
  int levels_ = 0;
  int width_ = 0;
  int height_ = 0;
  // Tile grid: size and origin.
  int tile_w_ = 0;
  int tile_h_ = 0;
  int tile_x0_ = 0;
  int tile_y0_ = 0;
  ColorFormat format_ = ColorFormat::Gray8;
};

// ----------------------------------------------------------------------------
// Encoder

void validate_j2k_options(const EncodeOptions& o) {
  if (o.levels < 1 || o.levels > kMaxLevels) {
    throw std::invalid_argument("levels must be in 1..16");
  }
  if (o.j2k_tile < 16 || o.j2k_cblk < 4 || o.j2k_cblk > 64 ||
      (o.j2k_cblk & (o.j2k_cblk - 1))) {
    throw std::invalid_argument(
        "j2k_tile must be >= 16 and j2k_cblk a power of two in 4..64");
  }
  if ((o.j2k_tile >> (o.levels - 1)) < 1) {
    throw std::invalid_argument("j2k_tile too small for this many levels");
  }
  if (!o.j2k_lossless && o.j2k_psnr <= 0) {
    throw std::invalid_argument("lossy J2K needs j2k_psnr > 0");
  }
  if (o.threads < 0) {
    throw std::invalid_argument(
        "threads must be >= 0 (0 = one per hardware thread)");
  }
}

opj_cparameters_t make_encoder_params(const EncodeOptions& o, int channels) {
  opj_cparameters_t p;
  opj_set_default_encoder_parameters(&p);
  p.tcp_numlayers = 1;
  if (o.j2k_lossless) {
    p.tcp_rates[0] = 0;
    p.cp_disto_alloc = 1;
    p.irreversible = 0;
  } else {
    p.tcp_distoratio[0] = o.j2k_psnr;
    p.cp_fixed_quality = 1;
    p.irreversible = 1;
  }
  p.numresolution = o.levels;
  p.cblockw_init = o.j2k_cblk;
  p.cblockh_init = o.j2k_cblk;
  p.tile_size_on = OPJ_TRUE;
  p.cp_tdx = o.j2k_tile;
  p.cp_tdy = o.j2k_tile;
  p.tcp_mct = channels == 3 ? 1 : 0;
  return p;
}

// Planar OpenJPEG copy of `src`.
ImagePtr make_opj_image(const Image& src) {
  const int channels = src.channels();
  std::vector<opj_image_cmptparm_t> params(static_cast<size_t>(channels));
  for (auto& c : params) {
    std::memset(&c, 0, sizeof(c));
    c.dx = c.dy = 1;
    c.w = OPJ_UINT32(src.width);
    c.h = OPJ_UINT32(src.height);
    c.prec = 8;
    c.sgnd = 0;
  }

  const OPJ_COLOR_SPACE space =
      channels == 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
  ImagePtr img(opj_image_create(OPJ_UINT32(channels), params.data(), space));
  if (!img) {
    throw std::runtime_error("opj_image_create failed");
  }
  img->x0 = img->y0 = 0;
  img->x1 = OPJ_UINT32(src.width);
  img->y1 = OPJ_UINT32(src.height);

  for (int c = 0; c < channels; ++c) {
    OPJ_INT32* dst = img->comps[c].data;
    for (int y = 0; y < src.height; ++y) {
      const uint8_t* row = src.row(y);
      for (int x = 0; x < src.width; ++x) {
        dst[size_t(y) * size_t(src.width) + size_t(x)] =
            row[size_t(x) * size_t(channels) + size_t(c)];
      }
    }
  }
  return img;
}

}  // namespace

EncodeReport encode_j2k(const Image& src_in,
                        const std::string& path,
                        const EncodeOptions& o) {
  const auto start = Clock::now();
  validate_j2k_options(o);
  if (src_in.width <= 0 || src_in.height <= 0) {
    throw std::invalid_argument("empty source image");
  }

  const Image src = convert(src_in, o.color);
  opj_cparameters_t params = make_encoder_params(o, src.channels());
  const ImagePtr img = make_opj_image(src);

  Messages msgs;
  CodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
  set_handlers(codec.get(), &msgs);
  if (!opj_setup_encoder(codec.get(), &params, img.get())) {
    throw std::invalid_argument("J2K setup: " + msgs.last_error);
  }
  const char* extra[] = {"TLM=YES", nullptr};
  if (!opj_encoder_set_extra_options(codec.get(), extra)) {
    throw std::runtime_error("J2K: TLM option rejected");
  }
  // OpenJPEG's own thread pool. The output is byte-identical for every
  // thread count (tests/test_threads.cpp). OpenJPEG codes the code-blocks of
  // one tile at a time in parallel, so small tiles cannot use many threads
  // (on test_image.jpg with 256² tiles, 4 threads encode 2.3× faster than
  // 1 and 14 threads only 1.2×): use at most one thread per 4 code-blocks of
  // a tile. A library without thread support refuses: encode on this thread.
  const int64_t blocks_per_tile =
      int64_t(o.j2k_tile / o.j2k_cblk) * int64_t(o.j2k_tile / o.j2k_cblk);
  int threads = int(std::min<int64_t>(resolve_threads(o.threads),
                                      std::max<int64_t>(blocks_per_tile / 4, 1)));
  if (threads > 1 && !opj_codec_set_threads(codec.get(), threads)) {
    threads = 1;
  }

  StreamPtr stream(
      opj_stream_create_default_file_stream(path.c_str(), OPJ_FALSE));
  if (!stream) {
    throw IoError("cannot create '" + path + "'");
  }
  if (!opj_start_compress(codec.get(), img.get(), stream.get()) ||
      !opj_encode(codec.get(), stream.get()) ||
      !opj_end_compress(codec.get(), stream.get())) {
    throw std::runtime_error("J2K encode failed: " + msgs.last_error);
  }
  stream.reset();  // flush + close

  struct stat st;
  if (::stat(path.c_str(), &st) != 0) {
    throw IoError("cannot stat '" + path + "'");
  }
  EncodeReport report;
  report.file_bytes = uint64_t(st.st_size);
  report.encode_ms = elapsed_ms(start);
  report.threads = threads;
  return report;
}

std::unique_ptr<CropDecoder> open_j2k(const std::string& path,
                                      const DecoderOptions&) {
  return std::make_unique<J2kDecoder>(path);
}

}  // namespace prmd::internal
