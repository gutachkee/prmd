// JPEG2000 backend (OpenJPEG 2.5).
//
// Encoder: tiled codestream (.j2k), reversible 5/3 (lossless) or 9/7 (lossy, PSNR target),
// numresolution = levels so that reduce = L yields level L, TLM tile-length index.
//
// Decoder: OpenJPEG decodes only once per codec instance, so every crop creates a fresh
// codec. The main header is read from the file once at open() and served from RAM to each
// per-crop codec (its parse cost stays in the crop latency; its bytes are not re-read).
// Tile data comes from the file through a counting stream (pread), so DecodeStats reports
// real bytes and read calls, including OpenJPEG's own read-ahead (stream buffer 4 KiB).

#include "prmd/internal/j2k_codec.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openjpeg.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "prmd/errors.h"

namespace prmd::internal {

namespace {

constexpr OPJ_SIZE_T kStreamBuffer = 4096;

struct Messages {
  std::string last_error;
};
void on_error(const char* msg, void* user) {
  auto* m = static_cast<Messages*>(user);
  m->last_error = msg;
  while (!m->last_error.empty() && (m->last_error.back() == '\n')) m->last_error.pop_back();
}
void on_quiet(const char*, void*) {}

struct CodecDeleter {
  void operator()(opj_codec_t* c) const {
    if (c) opj_destroy_codec(c);
  }
};
struct StreamDeleter {
  void operator()(opj_stream_t* s) const {
    if (s) opj_stream_destroy(s);
  }
};
struct ImageDeleter {
  void operator()(opj_image_t* i) const {
    if (i) opj_image_destroy(i);
  }
};
using CodecPtr = std::unique_ptr<opj_codec_t, CodecDeleter>;
using StreamPtr = std::unique_ptr<opj_stream_t, StreamDeleter>;
using ImagePtr = std::unique_ptr<opj_image_t, ImageDeleter>;

// Read stream over a file: bytes below `cached.size()` come from RAM, the rest via pread.
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
  if (s->pos >= s->size) return OPJ_SIZE_T(-1);
  n = OPJ_SIZE_T(std::min<uint64_t>(n, s->size - s->pos));
  auto* out = static_cast<uint8_t*>(buf);
  OPJ_SIZE_T done = 0;
  const uint64_t cached = s->cached ? s->cached->size() : 0;
  if (s->pos < cached) {
    const OPJ_SIZE_T k = OPJ_SIZE_T(std::min<uint64_t>(n, cached - s->pos));
    std::memcpy(out, s->cached->data() + s->pos, k);
    done += k;
  }
  if (done < n) {
    ++s->file_reads;
    while (done < n) {
      const ssize_t r = ::pread(s->fd, out + done, n - done, off_t(s->pos + done));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      s->file_bytes_read += uint64_t(r);
      done += OPJ_SIZE_T(r);
    }
  }
  s->pos += done;
  return done == 0 ? OPJ_SIZE_T(-1) : done;
}
OPJ_OFF_T src_skip(OPJ_OFF_T n, void* user) {
  auto* s = static_cast<ReadSource*>(user);
  if (n < 0 && uint64_t(-n) > s->pos) return -1;
  s->pos = uint64_t(int64_t(s->pos) + n);
  return n;
}
OPJ_BOOL src_seek(OPJ_OFF_T n, void* user) {
  auto* s = static_cast<ReadSource*>(user);
  if (n < 0 || uint64_t(n) > s->size) return OPJ_FALSE;
  s->pos = uint64_t(n);
  return OPJ_TRUE;
}

StreamPtr make_read_stream(ReadSource* src) {
  StreamPtr s(opj_stream_create(kStreamBuffer, OPJ_TRUE));
  if (!s) throw std::runtime_error("opj_stream_create failed");
  opj_stream_set_user_data(s.get(), src, nullptr);
  opj_stream_set_user_data_length(s.get(), src->size);
  opj_stream_set_read_function(s.get(), src_read);
  opj_stream_set_skip_function(s.get(), src_skip);
  opj_stream_set_seek_function(s.get(), src_seek);
  return s;
}

CodecPtr make_decoder(Messages* msgs) {
  CodecPtr c(opj_create_decompress(OPJ_CODEC_J2K));
  if (!c) throw std::runtime_error("opj_create_decompress failed");
  opj_set_error_handler(c.get(), on_error, msgs);
  opj_set_warning_handler(c.get(), on_quiet, nullptr);
  opj_set_info_handler(c.get(), on_quiet, nullptr);
  opj_dparameters_t p;
  opj_set_default_decoder_parameters(&p);
  if (!opj_setup_decoder(c.get(), &p)) throw std::runtime_error("opj_setup_decoder failed");
  return c;
}

class J2kDecoder final : public CropDecoder {
 public:
  explicit J2kDecoder(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) throw IoError("cannot open '" + path + "': " + std::strerror(errno));
    struct stat st;
    if (::fstat(fd_, &st) != 0) throw IoError("fstat failed");
    file_bytes_ = uint64_t(st.st_size);

    ReadSource src{fd_, file_bytes_, 0, nullptr, 0, 0};
    Messages msgs;
    CodecPtr codec = make_decoder(&msgs);
    StreamPtr stream = make_read_stream(&src);
    opj_image_t* raw = nullptr;
    if (!opj_read_header(stream.get(), codec.get(), &raw))
      throw FormatError("J2K header: " + msgs.last_error);
    ImagePtr img(raw);
    open_bytes_ = src.file_bytes_read;

    opj_codestream_info_v2_t* info = opj_get_cstr_info(codec.get());
    if (!info) throw FormatError("J2K: no codestream info");
    levels_ = int(info->m_default_tile_info.tccp_info[0].numresolutions);
    tdx_ = int(info->tdx);
    tdy_ = int(info->tdy);
    tx0_ = int(info->tx0);
    ty0_ = int(info->ty0);
    opj_destroy_cstr_info(&info);
    const uint64_t main_end = main_header_length();

    if (img->x0 != 0 || img->y0 != 0) throw FormatError("J2K: non-zero image origin is not supported");
    width_ = int(img->x1);
    height_ = int(img->y1);
    if (img->numcomps == 1) {
      format_ = ColorFormat::Gray8;
    } else if (img->numcomps == 3) {
      format_ = ColorFormat::RGB8;
    } else {
      throw FormatError("J2K: unsupported component count " + std::to_string(img->numcomps));
    }
    for (OPJ_UINT32 c = 0; c < img->numcomps; ++c)
      if (img->comps[c].prec != 8 || img->comps[c].sgnd || img->comps[c].dx != 1 || img->comps[c].dy != 1)
        throw FormatError("J2K: only unsigned 8-bit, unsubsampled components are supported");

    header_.resize(size_t(main_end));
    for (size_t done = 0; done < header_.size();) {
      const ssize_t r = ::pread(fd_, header_.data() + done, header_.size() - done, off_t(done));
      if (r <= 0) throw IoError("J2K: cannot read main header");
      done += size_t(r);
    }
  }
  ~J2kDecoder() override {
    if (fd_ >= 0) ::close(fd_);
  }

  std::string name() const override { return "j2k-t" + std::to_string(tdx_); }

  // Length of the main header: walk marker segments from SOC (each is 0xFF xx + u16 length)
  // until the first SOT (0xFF90). Reads only the main header, never tile data.
  uint64_t main_header_length() const {
    uint64_t pos = 2;  // after SOC
    for (int guard = 0; guard < 4096; ++guard) {
      uint8_t m[4];
      if (::pread(fd_, m, 4, off_t(pos)) != 4) break;
      if (m[0] != 0xFF) break;
      if (m[1] == 0x90) return pos;  // SOT
      pos += 2 + ((uint64_t(m[2]) << 8) | m[3]);
    }
    throw FormatError("J2K: cannot locate the end of the main header");
  }
  int num_levels() const override { return levels_; }
  ColorFormat color_format() const override { return format_; }
  LevelInfo level_info(int level) const override {
    if (level < 0 || level >= levels_) throw std::out_of_range("level out of range");
    return LevelInfo{(width_ + (1 << level) - 1) >> level, (height_ + (1 << level) - 1) >> level};
  }
  uint64_t file_bytes() const override { return file_bytes_; }
  uint64_t open_bytes_read() const override { return open_bytes_; }

 protected:
  Image decode_region(int level, const Rect& r, DecodeStats& stats) override {
    ReadSource src{fd_, file_bytes_, 0, &header_, 0, 0};
    Messages msgs;
    CodecPtr codec = make_decoder(&msgs);
    StreamPtr stream = make_read_stream(&src);
    opj_image_t* raw = nullptr;
    if (!opj_read_header(stream.get(), codec.get(), &raw)) throw FormatError("J2K header: " + msgs.last_error);
    ImagePtr img(raw);
    // Full-resolution window of the level-L rect.
    const int x0 = r.x << level, y0 = r.y << level;
    const int x1 = std::min(r.right() << level, width_), y1 = std::min(r.bottom() << level, height_);
    if (!opj_set_decoded_resolution_factor(codec.get(), OPJ_UINT32(level)) ||
        !opj_set_decode_area(codec.get(), img.get(), x0, y0, x1, y1) ||
        !opj_decode(codec.get(), stream.get(), img.get()) || !opj_end_decompress(codec.get(), stream.get()))
      throw FormatError("J2K decode: " + msgs.last_error);

    Image out(r.w, r.h, format_);
    const int C = out.channels();
    for (int c = 0; c < C; ++c) {
      const opj_image_comp_t& comp = img->comps[c];
      if (int(comp.w) != r.w || int(comp.h) != r.h)
        throw FormatError("J2K decode: unexpected output size " + std::to_string(comp.w) + "x" +
                          std::to_string(comp.h));
      for (int y = 0; y < r.h; ++y) {
        uint8_t* dst = out.row(y);
        const OPJ_INT32* s = comp.data + size_t(y) * comp.w;
        for (int x = 0; x < r.w; ++x) {
          const int v = s[x];
          dst[size_t(x) * size_t(C) + size_t(c)] = uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
      }
    }
    const int tiles_x = ((x1 - 1 - tx0_) / tdx_) - ((x0 - tx0_) / tdx_) + 1;
    const int tiles_y = ((y1 - 1 - ty0_) / tdy_) - ((y0 - ty0_) / tdy_) + 1;
    stats.tiles_touched = tiles_x * tiles_y;
    stats.blocks_decoded = stats.tiles_touched;
    stats.blocks_per_level[size_t(level)] = stats.tiles_touched;
    stats.bytes_read = src.file_bytes_read;
    stats.read_calls = src.file_reads;
    return out;
  }

 private:
  int fd_ = -1;
  uint64_t file_bytes_ = 0, open_bytes_ = 0;
  std::vector<uint8_t> header_;
  int levels_ = 0, width_ = 0, height_ = 0, tdx_ = 0, tdy_ = 0, tx0_ = 0, ty0_ = 0;
  ColorFormat format_ = ColorFormat::Gray8;
};

}  // namespace

EncodeReport encode_j2k(const Image& src_in, const std::string& path, const EncodeOptions& o) {
  const auto t0 = std::chrono::steady_clock::now();
  if (o.levels < 1 || o.levels > 16) throw std::invalid_argument("levels must be in 1..16");
  if (o.j2k_tile < 16 || o.j2k_cblk < 4 || o.j2k_cblk > 64 || (o.j2k_cblk & (o.j2k_cblk - 1)))
    throw std::invalid_argument("j2k_tile must be >= 16 and j2k_cblk a power of two in 4..64");
  if ((o.j2k_tile >> (o.levels - 1)) < 1) throw std::invalid_argument("j2k_tile too small for this many levels");
  if (!o.j2k_lossless && o.j2k_psnr <= 0) throw std::invalid_argument("lossy J2K needs j2k_psnr > 0");
  if (src_in.width <= 0 || src_in.height <= 0) throw std::invalid_argument("empty source image");
  const Image src = convert(src_in, o.color);
  const int C = src.channels();

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
  p.tcp_mct = C == 3 ? 1 : 0;

  std::vector<opj_image_cmptparm_t> cm(static_cast<size_t>(C));
  for (auto& c : cm) {
    std::memset(&c, 0, sizeof(c));
    c.dx = c.dy = 1;
    c.w = OPJ_UINT32(src.width);
    c.h = OPJ_UINT32(src.height);
    c.prec = 8;
    c.sgnd = 0;
  }
  ImagePtr img(opj_image_create(OPJ_UINT32(C), cm.data(), C == 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY));
  if (!img) throw std::runtime_error("opj_image_create failed");
  img->x0 = img->y0 = 0;
  img->x1 = OPJ_UINT32(src.width);
  img->y1 = OPJ_UINT32(src.height);
  for (int c = 0; c < C; ++c) {
    OPJ_INT32* d = img->comps[c].data;
    for (int y = 0; y < src.height; ++y) {
      const uint8_t* s = src.row(y);
      for (int x = 0; x < src.width; ++x) d[size_t(y) * size_t(src.width) + size_t(x)] = s[size_t(x) * size_t(C) + size_t(c)];
    }
  }

  Messages msgs;
  CodecPtr codec(opj_create_compress(OPJ_CODEC_J2K));
  opj_set_error_handler(codec.get(), on_error, &msgs);
  opj_set_warning_handler(codec.get(), on_quiet, nullptr);
  opj_set_info_handler(codec.get(), on_quiet, nullptr);
  if (!opj_setup_encoder(codec.get(), &p, img.get())) throw std::invalid_argument("J2K setup: " + msgs.last_error);
  const char* extra[] = {"TLM=YES", nullptr};
  if (!opj_encoder_set_extra_options(codec.get(), extra)) throw std::runtime_error("J2K: TLM option rejected");
  StreamPtr stream(opj_stream_create_default_file_stream(path.c_str(), OPJ_FALSE));
  if (!stream) throw IoError("cannot create '" + path + "'");
  if (!opj_start_compress(codec.get(), img.get(), stream.get()) || !opj_encode(codec.get(), stream.get()) ||
      !opj_end_compress(codec.get(), stream.get()))
    throw std::runtime_error("J2K encode failed: " + msgs.last_error);
  stream.reset();  // flush + close

  EncodeReport rep;
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) throw IoError("cannot stat '" + path + "'");
  rep.file_bytes = uint64_t(st.st_size);
  rep.encode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return rep;
}

std::unique_ptr<CropDecoder> open_j2k(const std::string& path, const DecoderOptions&) {
  return std::make_unique<J2kDecoder>(path);
}

}  // namespace prmd::internal
