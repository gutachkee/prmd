# Prmd — design: random-access crop decoding from a single-file image pyramid

Status: draft v1 · Language: C++17 · Target: AArch64 Linux (the dev host is AArch64 too)

This document defines **all interfaces first** (§1–§4) and then the design behind them (§5–§11).
Every name in §1–§4 is normative: the code must match it. Sections after that explain *why*.

---

## 0. Scope

**Goals**
- Encode an 8-bit image of W×H into **one file** holding an N-level pyramid (level 0 = full resolution, level L = `ceil(W/2^L) × ceil(H/2^L)`). The color format is chosen at encode time: **Gray8** (required by the task) or **RGB8** (optional in the task, supported here).
- `decode(level, x, y, w, h)` returns exactly those pixels, reading and decoding only the data the crop needs, located through an index (no scanning).
- Compare three ways of defining and storing levels against a naive tiled pyramid and JPEG2000, using measured numbers.

**Non-goals (v1)**
- Other pixel formats (16-bit, alpha, CMYK). Alpha is dropped on load.
- Thread-safe sharing of a single decoder instance.
- Rate-distortion-optimised lossy coding. Lossy `.prmd` (§5.2) uses a plain uniform quantizer with one step `quant`; there is no rate control or PSNR target.

**Codecs and modes**

| id | Container | Level definition | Storage per finer level |
|---|---|---|---|
| `haar` | `.prmd` | A: exact 2×2 mean, floor of floors | 3 Haar detail bands |
| `cdf53` | `.prmd` | B: 5/3 wavelet lowpass (as in lossless JPEG2000) | 3 5/3 detail bands |
| `laplacian-box` | `.prmd` | C: rounded 2×2 mean `(a+b+c+d+2)>>2` | residual vs upsampled coarser level |
| `laplacian-gauss` | `.prmd` | C: 5-tap Gaussian + decimate (pyrDown-like) | residual vs upsampled coarser level |
| `naive` | `.prmd` | rounded 2×2 mean (same as laplacian-box) | each level stored independently |
| `j2k` | `.j2k` codestream | J2K 5/3 LL band | OpenJPEG, tiled, TLM index |

---

## 1. Public C++ API (`include/prmd/`)

Everything lives in `namespace prmd`.

### 1.1 `image.h`: basic types
```cpp
namespace prmd {

// Pixel rectangle: top-left corner (x, y), size w × h. Covers columns x .. x+w−1
// and rows y .. y+h−1.
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  int right()  const { return x + w; }      // one past the last column (exclusive end)
  int bottom() const { return y + h; }      // one past the last row (exclusive end)
};
Rect intersect(const Rect& a, const Rect& b);  // returns an empty Rect if they don't overlap

// Pixel format identifier. The numeric values are stable on-disk ids, NOT channel counts:
// several formats may share a channel count (e.g. a future BGR8 or YCbCr8 alongside RGB8).
// New formats get the next free id. Existing ids never change.
enum class ColorFormat : uint8_t {
  Gray8 = 0,   // 1 channel: luma
  RGB8  = 1,   // 3 channels: R, G, B (in that order)
};

// Per-format properties come from one table, so adding a format means adding one row.
struct ColorFormatInfo {
  ColorFormat format;
  const char* name;      // "gray", "rgb" (CLI spelling)
  int         channels;
};
const ColorFormatInfo& info(ColorFormat f);   // throws std::invalid_argument for unknown ids
inline int channels(ColorFormat f) { return info(f).channels; }

// 8-bit image, row-major, channels interleaved (RGBRGB…), tightly packed:
// stride = width * channels.
struct Image {
  int width = 0, height = 0;
  ColorFormat format = ColorFormat::Gray8;
  std::vector<uint8_t> pixels;

  Image() = default;
  Image(int w, int h, ColorFormat f = ColorFormat::Gray8);   // zero-filled
  int            channels() const { return prmd::channels(format); }
  size_t         stride()   const { return size_t(width) * channels(); }
  uint8_t*       row(int y)       { return pixels.data() + y * stride(); }
  const uint8_t* row(int y) const { return pixels.data() + y * stride(); }
  // Sample at column x, row y, channel `channel`. The channel index follows the format's
  // channel order: Gray8 has only 0; RGB8 has 0 = R, 1 = G, 2 = B.
  uint8_t  at(int x, int y, int channel = 0) const { return row(y)[x * channels() + channel]; }
  Image crop(const Rect& r) const;           // plain copy; throws std::out_of_range
};

// Format conversion. RGB→Gray uses BT.601 integer (77R + 150G + 29B + 128) >> 8.
// Gray→RGB replicates the value into all three channels.
Image convert(const Image& src, ColorFormat to);

}  // namespace prmd
```

### 1.2 `image_io.h`: file I/O
```cpp
namespace prmd {
// Loads in the file's native format: grayscale JPEG/PNG → Gray8; color → RGB8.
// .jpg/.jpeg: libjpeg-turbo (JCS_GRAYSCALE or JCS_RGB). .png: libpng (alpha is dropped,
// palette is expanded, 16-bit is rejected). The encoder converts to EncodeOptions::color.
// Throws prmd::IoError on failure.
Image load_image(const std::string& path);
void  save_png(const std::string& path, const Image& img);   // Gray8 or RGB8 PNG
}
```

### 1.3 `crop_decoder.h`: the common decoding interface
```cpp
namespace prmd {

// kMaxLevels (= 16) comes from encoder.h (§1.4), which this header includes.

struct LevelInfo { int width = 0, height = 0; };

// Statistics for one decode() call. Every codec fills these in the same way.
struct DecodeStats {
  int      tiles_touched = 0;    // distinct (level, 256x256 tile) pairs whose blocks were used
                                 //   (J2K: distinct J2K tiles intersecting the decode window)
  int      blocks_decoded = 0;   // independently entropy-coded units decoded, all levels
  std::array<int, kMaxLevels> blocks_per_level{};   // the same count, broken down by level
  uint64_t bytes_read = 0;       // file bytes fetched by this call (excludes open())
  int      read_calls = 0;       // number of pread()/stream reads issued
  int      cache_hits = 0;       // blocks served from the LRU cache (0 when the cache is off)
};

// What decode() returns: the pixels, the rect they cover (== the request for decode(),
// the clamped intersection for decode_clamped()), and the cost of producing them.
struct DecodeResult {
  Image       image;
  Rect        rect;
  DecodeStats stats;
};

struct DecoderOptions {
  size_t cache_bytes = 0;        // LRU budget for decoded blocks; 0 = disabled
};

class CropDecoder {
 public:
  virtual ~CropDecoder() = default;

  virtual std::string name() const = 0;              // e.g. "prmd/haar", "j2k"
  virtual int         num_levels() const = 0;
  virtual ColorFormat color_format() const = 0;    // as encoded; decode() returns this format
  virtual LevelInfo   level_info(int level) const = 0;
  virtual uint64_t    file_bytes() const = 0;
  virtual uint64_t    open_bytes_read() const = 0;   // header + index read at open

  // Public entry points are non-virtual (the NVI pattern). They validate the request
  // in one place for every codec, then call decode_region().

  // Strict: requires 0 <= level < num_levels(), r.w > 0, r.h > 0, r.x >= 0, r.y >= 0,
  // r.x + r.w <= width(level), r.y + r.h <= height(level), checked without int overflow
  // (huge coordinates are rejected, never wrapped). Otherwise throws std::out_of_range.
  // Usage: dec.decode(2, {x, y, w, h});
  DecodeResult decode(int level, const Rect& r);

  // Clamping variant: intersects r with the level and decodes the intersection
  // (DecodeResult::rect). Throws std::out_of_range if the intersection is empty or the
  // level is invalid.
  DecodeResult decode_clamped(int level, const Rect& r);

 protected:
  // Codec-specific work. Called only with a validated level and a non-empty in-bounds rect.
  // Must fill `stats`.
  virtual Image decode_region(int level, const Rect& r, DecodeStats& stats) = 0;
};
// Not thread-safe (§0): decode() updates the block cache without locking. Use one
// decoder per thread.

// Picks the codec by file magic: "PRMD" means .prmd, FF 4F FF 51 (SOC+SIZ) means J2K codestream.
// Throws prmd::FormatError for unknown or corrupt files and prmd::IoError if the file can't be opened.
std::unique_ptr<CropDecoder> open_decoder(const std::string& path,
                                          const DecoderOptions& opts = {});
}
```

### 1.4 `encoder.h`: encoding entry point
```cpp
namespace prmd {

constexpr int kMaxLevels = 16;

enum class Codec       : uint8_t { Prmd, J2K };
enum class Transform   : uint8_t { Haar = 0, Cdf53 = 1, Laplacian = 2, Independent = 3 };  // Independent = naive
enum class Downsampler : uint8_t { Box = 0, Gauss = 1 };   // used only by Laplacian and Independent
// How code-block numbers become bytes (§7). Values are stable on-disk ids.
enum class Entropy     : uint8_t {
  Range = 0,   // custom context-adaptive binary range coder: best ratio, ~12× slower decode (~10× on a Pi 5)
  Zstd  = 1,   // zigzag int16 byte planes + zstd: DEFAULT (§7.1) — fast decode, a few % larger
  Raw   = 2,   // zigzag int16, uncompressed: no entropy coding, for measurement only
};

struct EncodeOptions {
  int levels = 5;                    // N, 1..kMaxLevels; each level must be ≥1x1
  ColorFormat color = ColorFormat::Gray8;   // stored format. The source is converted with
                                            // prmd::convert() if it differs (RGB→Gray via BT.601,
                                            // Gray→RGB by replication).
  // .prmd
  Transform   transform   = Transform::Haar;
  Downsampler downsampler = Downsampler::Box;
  int tile = 256;                    // reporting/grouping unit; multiple of cblk
  int cblk = 128;                    // code-block = independently decodable unit; even, ≥16
  int quant = 0;                     // 0 = lossless; Q > 0 = lossy step (§5.2); not for naive
  Entropy entropy = Entropy::Zstd;   // block payload coder (§7); default chosen in §7.1
  int entropy_level = 0;             // coder-specific effort; 0 = coder default.
                                     //   Zstd: 1..22 (default 1). Range, Raw: must be 0.
  bool med = true;                   // MED on base blocks (§3.3); false stores pixel values
  bool ycocg = true;                 // YCoCg-R on RGB (§5.1); false stores R, G, B. Ignored
                                     //   for gray. Both are recorded in header byte 30 (§3.1)
  int threads = 0;                   // encoder threads (.prmd and J2K): 0 = one per hardware
                                     //   thread, 1 = serial (no thread started), N = N threads.
                                     //   < 0 rejected, > 256 clamped. The file does not depend
                                     //   on it (see "Threads" below)
  // J2K
  int   j2k_tile = 256;              // full-resolution tile size (benchmarked: 256, 1024)
  int   j2k_cblk = 64;
  bool  j2k_lossless = true;         // 5/3 reversible; false means 9/7 + j2k_psnr
  float j2k_psnr = 0.f;              // target PSNR (dB) for lossy
};

struct EncodeReport {
  uint64_t file_bytes = 0;
  std::array<uint64_t, kMaxLevels> level_bytes{};  // payload bytes attributed to each level
  double   encode_ms = 0;
  int      threads = 1;              // threads the encode ran on, the caller included
};

EncodeReport encode_file(const Image& src, const std::string& out_path,
                         Codec codec, const EncodeOptions& opts);
}
```

**Threads.** `encode_file` can use several threads; decoding is single-threaded (§11 L2).

- *Output.* A `.prmd` file is **byte-identical for every `threads` value**: the format already
  makes every code-block a pure function of the stored planes, entropy-coded with fresh state
  (§2.4), and everything before block coding is per sample or per row of a level. Threads only
  change who computes which piece; each piece writes its own slot, and the file is assembled in
  block-id order. There is no format change and no version bump. Tests pin the serial output to
  golden hashes of the encoder before threading and compare every thread count against it (§9).
- *Default.* `threads = 0` uses `std::thread::hardware_concurrency()` (1 if unknown). This
  default makes the library use all cores of the host, a change for embedders that call
  `encode_file` from their own worker threads: pass `threads = 1` there. The CLI has `--threads`
  (§4.1); `prmdbench` and `prmdpipeline` default to all hardware threads too; their
  published single-core numbers were taken with `--threads 1`.
- *Count used.* `.prmd` uses at most one thread per present code-block (an image with a single
  block runs serially) and at most 256; `EncodeReport::threads` says how many it used. With one
  thread no pool exists: every stage runs its loop once on the calling thread, no thread is
  started.
- *Mechanism* (`internal/parallel.h`, §2.8). One fixed-size `Pool` per call (the caller plus
  `threads − 1` workers), so concurrent `encode_file` calls from different user threads stay
  independent. Stages: level-0 planes straight from the source (colour conversion fused in) by
  row; `forward` (Haar, Cdf53) and the box/Gauss downsamplers by row stripes of the output level;
  YCoCg-R and quantization in runs of 16 Ki samples; the Laplacian closed loop serially over
  levels, each level's upsample by rows and its residual/reconstruction pass by sample runs; then
  code-blocks, one per job, coded into exact-size payloads. Offsets are summed in id order and the
  payloads are streamed to the file. Planes that a pass overwrites entirely are allocated without
  zero-filling (`Region(rect, kNoInit)`, §2.1), so their pages are first touched by the threads
  that write them.
- *Errors.* Jobs are claimed in increasing index order. If any fail, the exception of the
  lowest failing index is rethrown once all threads are idle, the same exception (type and
  message) that the serial loop throws first, at every thread count. The output file is created
  only after every block is coded, so a failed encode leaves no file (and an existing file at
  `out_path` untouched). The one ordering difference: the 4 GiB data-size check runs after all
  blocks are coded, so a block coder error in a later block wins over it.
- *J2K.* `threads` is passed to `opj_codec_set_threads` (not called for 1, as before). OpenJPEG's
  output is byte-identical for every thread count (tested on gray, RGB, lossless and lossy).
  OpenJPEG parallelizes the code-blocks of one tile at a time, so small tiles cannot use many
  threads: the encoder uses at most `(j2k_tile / j2k_cblk)² / 4` threads, i.e. 4 for the default
  256² tiles with 64² code-blocks (on `test_image.jpg` 14 threads were slower than 4 there), 14
  of 14 for 1024² tiles.
- *Profiling.* With `PRMD_PROFILE`, steps that run on workers are merged into the calling
  thread's profile tree; their times are summed over threads (`profile.h`, "Threads").

### 1.5 `pyramid.h`: the normative level definitions
```cpp
namespace prmd {
// Returns `levels` images; [0] is `src`, [L] is ceil(W/2^L) × ceil(H/2^L).
// RGB8: every filter is applied per channel independently.
//   Haar:        ⌊(⌊(a+b)/2⌋ + ⌊(c+d)/2⌋)/2⌋ (odd dims: last row/col replicated)
//   Cdf53:       5/3 LL band of the previous level's *unclamped* integer LL;
//                each returned level is that LL clamped to [0, 255] (§5)
//   Laplacian/Independent: Box (a+b+c+d+2)>>2, or Gauss [1 4 6 4 1]/16 reflect-101 (s+128)>>8
// Decoded crops must equal crops of these images exactly. Tests check this against their
// own independent re-implementation (tests/reference.cpp).
std::vector<Image> build_pyramid(const Image& src, int levels, Transform t, Downsampler d);
}
```
There are no public single-step `downsample_*` functions. For Cdf53 a level is not a function of the previous *8-bit* level, because the chain carries unclamped integers. A single-step API would therefore be misleading.

### 1.6 Errors (`errors.h`)
```cpp
namespace prmd {
struct IoError     : std::runtime_error { using runtime_error::runtime_error; };  // open/read/write
struct FormatError : std::runtime_error { using runtime_error::runtime_error; };  // bad magic/version/field values/index/payload
// Bad level or rect: std::out_of_range. Bad EncodeOptions: std::invalid_argument.
}
```

---

## 2. Internal interfaces (`include/prmd/internal/`)

These are not part of the public API, but they are fixed here so the components can be built and tested independently. Everything here lives in `namespace prmd::internal` (C++17 nested namespace). Public types such as `Rect`, `Image` and `LevelInfo` are visible unqualified, because `prmd` encloses it. Public headers never include `internal/` headers, and tests may use them.

### 2.1 `region.h`: integer sample regions
```cpp
namespace prmd::internal {
// A rectangle of int32 samples, addressed in the coordinates of the plane it belongs to.
// std::vector<int32_t> whose allocator can skip value-initialization (SampleAllocator): it zeroes
// new elements like std::vector unless created by uninitialized_samples(n).
using Samples = std::vector<int32_t, SampleAllocator<int32_t>>;
Samples uninitialized_samples(size_t n);   // values unspecified: write every one before reading

struct Region {
  Rect rect;                         // position inside the owning plane
  Samples v;                         // rect.w * rect.h, row-major
  explicit Region(const Rect& r);    // zero-filled
  Region(const Rect& r, NoInit);     // unspecified samples, for a plane a pass writes entirely
  int32_t& at(int x, int y);         // absolute plane coordinates
  int32_t  at(int x, int y) const;
  int32_t* row(int y);               // start of row y (absolute); element i is column rect.x + i
  bool covers(const Rect& r) const;
};
void copy_overlap(const Region& src, Region& dst);   // same plane coordinates
}
```

### 2.1b `planes.h`: Image ↔ int32 planes
```cpp
namespace prmd::internal {
std::vector<Region> to_planes(const Image& img);                       // one plane per channel
// to_planes(convert(img, stored)) in one pass, rows split over `pool` (encoder level 0).
std::vector<Region> to_planes(const Image& img, ColorFormat stored, Pool* pool);
Image from_planes(const std::vector<Region>& planes, ColorFormat f);   // clamps to [0, 255]
// [level][channel] integer chain (unclamped; matters only for Cdf53).
std::vector<std::vector<Region>> build_level_planes(const Image& src, int levels, Transform t, Downsampler d);
}
```

### 2.2 `transform.h`: per-mode transform (encoder side and region inverse)
```cpp
namespace prmd::internal {

enum class Band : uint8_t { Base = 0, LH = 1, HL = 2, HH = 3, Residual = 4 };
struct Dims { int w = 0, h = 0; };
inline Dims half_up(Dims d);                           // ceil(w/2), ceil(h/2)

// All planes are whole-plane Regions with origin (0,0). `fine` is the true level size.
// Haar/Cdf53 pad odd sizes to even internally (replicate the last row/col).
class LevelTransform {
 public:
  virtual ~LevelTransform() = default;

  virtual int  num_bands() const = 0;                  // 3 (Haar, Cdf53) or 1 (Laplacian)
  virtual Band band_kind(int i) const = 0;
  virtual Dims band_size(Dims fine) const = 0;         // half_up(fine) or fine
  // Band samples belonging to the code-block whose pixel rect is fine_r (block → band mapping).
  virtual Rect band_rect_of_block(const Rect& fine_r, Dims fine) const = 0;

  // Encoder: full fine level → full coarse level (half_up(fine)) + full bands. With a pool,
  // rows are split over its threads; the result is the same for every pool and for none.
  virtual void forward(const Region& fine, Region& coarse, std::vector<Region>& bands,
                       Pool* pool = nullptr) const = 0;

  // Decoder: coarse / band samples needed to rebuild fine rect r, already clipped to the planes.
  virtual Rect coarse_needed(const Rect& fine_r, Dims fine) const = 0;
  virtual Rect band_needed(const Rect& fine_r, Dims fine) const = 0;

  // Decoder: rebuild exactly fine_r (out.rect == fine_r) from regions covering *_needed.
  virtual void inverse(const Region& coarse, const std::vector<Region>& bands, const Rect& fine_r,
                       Dims fine, Region& out) const = 0;
};

// Throws std::invalid_argument for Transform::Independent (it has no level transform).
std::unique_ptr<LevelTransform> make_transform(Transform t, Downsampler d);
Region downsample_box(const Region& fine, Pool* pool = nullptr);     // Laplacian-box and naive
Region downsample_gauss(const Region& fine, Pool* pool = nullptr);   // Laplacian-gauss
// Laplacian prediction U(coarse) of a whole level: the inverse with a zero residual.
void laplacian_upsample(Downsampler d, const Region& coarse, Dims fine, Region& out,
                        Pool* pool = nullptr);
}
```

### 2.3 `rangecoder.h`: adaptive binary range coder (header-only, so the hot loop inlines)
```cpp
namespace prmd::internal {
using Prob = uint16_t;                        // P(bit==0) in 1/4096 units
constexpr Prob kProbInit = 2048;
constexpr int  kProbBits = 12, kAdaptShift = 5;

class RangeEncoder {
 public:
  explicit RangeEncoder(std::vector<uint8_t>& out);
  void encode_bit(Prob& p, int bit);          // adaptive
  void encode_direct(uint32_t value, int nbits);  // equiprobable bits, nbits ≤ 24
  void finish();                              // flushes; must be called exactly once
};

class RangeDecoder {
 public:
  RangeDecoder(const uint8_t* data, size_t size);   // reading past the end yields 0s (no UB)
  int      decode_bit(Prob& p);
  uint32_t decode_direct(int nbits);
  size_t   overrun() const;                         // bytes requested past the end (truncation check)
};
}
```

### 2.4 `blockcoder.h`: code-block payload coding
```cpp
namespace prmd::internal {
// One code-block = (bands per channel) × (channels) planes of signed ints with known dims.
// Plane order: for each band, for each channel (e.g. RGB haar: LH.Y LH.Co LH.Cg HL.Y …).
// For RGB8 the planes hold YCoCg-R-transformed values (§5.1), and the channel index selects
// a separate context set (luma vs. chroma).
struct BlockPlanes {
  int w = 0, h = 0;                           // per-plane dims (identical for all planes in a block)
  int channels = 1;                           // 1 or 3
  std::vector<Band> kinds;                    // one per plane
  std::vector<Samples> planes;                // each w*h, row-major
};

// Reversible color decorrelation (YCoCg-R) applied in place to channel triplets of any
// signed ints: pixels (Base) or detail/residual coefficients.
void ycocg_r_forward(int32_t* c0, int32_t* c1, int32_t* c2, size_t n);
void ycocg_r_inverse(int32_t* c0, int32_t* c1, int32_t* c2, size_t n);

// Pluggable entropy coder for one code-block (§7). Implementations are stateless between
// calls: all adaptive state is created fresh per block, so every block is independently
// decodable whichever coder is chosen. Thread-safe (const methods, no shared state).
class BlockCoder {
 public:
  virtual ~BlockCoder() = default;
  virtual Entropy id() const = 0;
  virtual void encode(const BlockPlanes& in, std::vector<uint8_t>& out) const = 0;
  // `io` arrives with w, h, channels and kinds filled in; the planes get resized and filled.
  // Throws FormatError on a payload that is truncated or inconsistent.
  virtual void decode(const uint8_t* data, size_t size, BlockPlanes& io) const = 0;
};

// level: EncodeOptions::entropy_level (ignored by decoders, since the payload is self-describing).
// Throws std::invalid_argument for an unknown id or invalid level.
std::unique_ptr<BlockCoder> make_block_coder(Entropy e, int level = 0);

// MED (LOCO-I) prediction for Band::Base on int32 samples, causal within the w×h block only
// (first sample predicted as 0). Applied after YCoCg-R for RGB. Skipped when the file's
// no-MED flag is set (§3.1).
void med_residual(const int32_t* px, int w, int h, int32_t* res);
void med_reconstruct(const int32_t* res, int w, int h, int32_t* px);
}
```

### 2.5 `prmd_format.h`: on-disk structures (§3 gives the byte layout)
```cpp
namespace prmd::internal {
struct PrmdHeader { /* fields in §3.1 */ };
struct LevelEntry { uint32_t width, height; uint16_t blocks_x, blocks_y; uint32_t first_block; };
// Derived geometry shared by encoder and decoder (the level table must agree with it).
struct LevelGeom { int w, h, blocks_x, blocks_y, tiles_x, tiles_y, k; uint32_t first_block, num_ids; bool base; };
std::vector<LevelGeom> make_geometry(int width, int height, int levels, int tile, int cblk, Transform t);
uint32_t block_id(const LevelGeom& g, int bx, int by);        // §3.3
int      tile_index(const LevelGeom& g, int bx, int by);
Rect     block_pixel_rect(const LevelGeom& g, int cblk, int bx, int by);
// Explicit little-endian (de)serialization. No packed-struct memcpy.
std::vector<uint8_t> serialize(PrmdHeader& h, const std::vector<LevelEntry>&, const std::vector<uint32_t>& offsets);
// nullptr if the geometry is valid, else a reason. 64-bit arithmetic; shared by encoder and
// decoder: dims ≤ 2^30, cblk even 16..4096, tile = k·cblk ≤ 32768, blocks per row/column
// ≤ 65535 (u16 level table), total block ids < 2^32 − 1.
const char* geometry_error(uint64_t width, uint64_t height, int levels, int tile, int cblk);
PrmdHeader parse_header(const uint8_t* buf, size_t n);   // magic, version, ids, reserved = 0,
                                                         // geometry_error, total_blocks, data_offset
size_t     index_bytes(const PrmdHeader& h);             // level table + offsets size
void parse_index(const PrmdHeader& h, const uint8_t* buf, size_t n, std::vector<LevelEntry>&,
                 std::vector<uint32_t>& offsets);        // monotonic offsets, table == geometry
}
```

### 2.6 `j2k_codec.h`: OpenJPEG backend
```cpp
namespace prmd::internal {
EncodeReport encode_j2k(const Image& src, const std::string& path, const EncodeOptions& opts);
std::unique_ptr<CropDecoder> open_j2k(const std::string& path, const DecoderOptions& opts);
}
```

### 2.7 `prmd_codec.h`: .prmd entry points
```cpp
namespace prmd::internal {
// coder: replaces the opts.entropy coder (tests inject failures); null = the normal one.
EncodeReport encode_prmd(const Image& src, const std::string& path, const EncodeOptions& opts,
                         const BlockCoder* coder = nullptr);
std::unique_ptr<CropDecoder> open_prmd(const std::string& path, const DecoderOptions& opts);
void validate_prmd_options(const EncodeOptions& opts);   // std::invalid_argument
}
```

### 2.8 `parallel.h`: the encoder's thread pool
```cpp
namespace prmd::internal {
constexpr int kMaxThreads = 256;
// 0 → hardware (1 if 0); positive values as given; clamped to [1, kMaxThreads]; < 0 throws.
int resolve_threads(int requested, unsigned hardware);
int resolve_threads(int requested);                       // hardware_concurrency()

class Pool {                       // threads − 1 workers + the caller; joined by the destructor
 public:
  explicit Pool(int threads);      // keeps the workers it could start if the system refuses more
  int size() const;
  // fn(begin, end) over chunks of [0, n), `grain` long, claimed in increasing order from an
  // atomic counter; blocks until all are done. Inline fn(0, n) when n <= grain, without
  // workers, or nested in another run(). Exceptions: unclaimed chunks are skipped, the lowest
  // failing chunk's exception is rethrown (= the serial loop's first). Workers' PRMD_TIMED
  // scopes are merged under the caller's open scope.
  void run(size_t n, size_t grain, const std::function<void(size_t, size_t)>& fn);
};
// fn(0, n) / fn(0, h) when pool is null (the serial encoder), else pool->run.
void parallel_for(Pool*, size_t n, size_t grain, fn);
void parallel_rows(Pool*, int h, int width, fn);         // ~16 Ki samples per row stripe
void parallel_samples(Pool*, size_t n, fn);              // 16 Ki-sample runs
}
```
Not work-stealing: all parallel work is flat (one level of `run`), so a shared counter is
enough. No global state; a pool lives for one `encode_prmd` call.

---

## 3. File format `.prmd` (version 1, all integers little-endian)

```
offset 0  ┌────────────────────────────────────────────┐
          │ Header (64 B)                              │
          ├────────────────────────────────────────────┤
          │ Level table: N × LevelEntry (16 B each)    │  level 0 … N−1
          ├────────────────────────────────────────────┤
          │ Block index: u32 offsets[total_blocks + 1] │  relative to data_offset;
          │                                            │  size(b) = off[b+1] − off[b]
          ├────────────────────────────────────────────┤ ← data_offset
          │ Level N−1 blocks (Base)                    │  coarsest first
          │ Level N−2 blocks (detail / residual)       │
          │   …                                        │
          │ Level 0 blocks                             │
          └────────────────────────────────────────────┘
```

### 3.1 Header (64 bytes)
| off | type | field | notes |
|---|---|---|---|
| 0 | char[4] | magic | `"PRMD"` |
| 4 | u16 | version | 1 |
| 6 | u8 | transform | 0 haar, 1 cdf53, 2 laplacian, 3 independent |
| 7 | u8 | downsampler | 0 box, 1 gauss (ignored for haar/cdf53) |
| 8 | u32 | width | level 0 |
| 12 | u32 | height | level 0 |
| 16 | u8 | levels | N |
| 17 | u8 | color | `ColorFormat` id: 0 = Gray8, 1 = RGB8. The channel count is derived from it; an unknown id raises `FormatError` |
| 18 | u16 | tile | 256 |
| 20 | u16 | cblk | 128 |
| 22 | u16 | quant | 0 = lossless; Q > 0 = quantizer step of the detail/residual bands (§5.2). Must be 0 for `independent` |
| 24 | u32 | total_blocks | |
| 28 | u8 | entropy | `Entropy` id: 0 = range, 1 = zstd, 2 = raw. An unknown id raises `FormatError` |
| 29 | u8 | entropy_level | informational (as encoded; 0 = coder default) |
| 30 | u8 | flags | bit 0 = no MED (base blocks store pixel values), bit 1 = no YCoCg-R (RGB stores R, G, B); 0 = both on. Other bits must be 0, and bit 1 must be 0 for gray |
| 31 | u8 | reserved | 0 |
| 32 | u64 | data_offset | |
| 40 | u8[24] | reserved | 0 |

There is no checksum. On open the decoder validates the header fields (reserved bytes and unknown flag bits must be 0, and gray files must not set no-YCoCg-R; geometry via `geometry_error`, in 64-bit; `total_blocks` must equal the count derived from the geometry, checked before the index is allocated; `data_offset` must not exceed the file size before the index is read), the level table (must equal the geometry derived from the header), the offsets (start at 0, monotonic) and the file size (must equal `data_offset` + last offset). This stops a damaged file from crashing the decoder or reading outside the file. Detecting bit-level corruption is left to the storage layer: a checksum on the index alone would protect 5 KB of an 11 MB file, and a damaged payload can still decode to wrong pixels without an error.

### 3.2 Level entry (16 bytes)
`u32 width, u32 height, u16 blocks_x, u16 blocks_y, u32 first_block` plus an implicit kind: `Base` for level N−1 (and for every level when `transform = independent`), otherwise `Detail`.

### 3.3 Blocks and tiles
- A **code-block** covers `cblk × cblk` pixels of *its own level* (edge blocks are smaller). It is the unit of entropy coding, I/O and caching.
  - Base block: 1 plane per channel of the block's pixel size: MED residuals, or the pixel values themselves when the file's no-MED flag is set (`--no-med`).
  - Detail block, haar/cdf53: 3 planes (LH, HL, HH) per channel, each `ceil(bw/2) × ceil(bh/2)`.
  - Detail block, laplacian: 1 plane (Residual) per channel, `bw × bh`.
  - All channels of an area live in the **same** block, so RGB needs no extra index entries or reads.
- A **tile** is a `tile × tile` pixel area = `(tile/cblk)²` blocks. Tiles are the reporting unit from the task, and blocks are stored **tile-major**, so a tile's blocks are contiguous on disk:
  `block_id = first_block + (ty*tiles_x + tx) * k² + (by%k)*k + (bx%k)`, where `k = tile/cblk`. The formula is O(1) and needs no lookup table. Edge tiles can have fewer than k² real blocks; the ids of the missing ones still exist in the index with **zero length** (4 bytes each), so the id space stays a simple formula.
- **Padding:** before the forward transform, each level is padded to even dimensions by replicating the last column and row. The padded samples are never returned.

### 3.4 Block payload
The output of `BlockCoder::encode` for the coder named in the header (§7). There is no per-block header, because dims and kinds follow from the level table and the coder from the file header. All blocks in a file use the same coder.

---

## 4. Command-line interfaces

### 4.1 `prmd` (CLI executable)
```
prmd encode <in.jpg|in.png> <out> [--codec prmd|j2k] [--color gray|rgb]   (default gray)
           [--mode haar|cdf53|laplacian|naive] [--down box|gauss]
           [--levels 5] [--tile 256] [--cblk 128] [--quant Q]
           [--entropy zstd|range|raw] [--entropy-level N]   (default zstd, level 1)
           [--no-med] [--no-ycocg]                           (.prmd only; default both on)
           [--threads N]                                     (default 0 = all hardware threads)
           [--j2k-tile 256] [--j2k-lossy-psnr DB]
        → prints EncodeReport (size, ratio vs raw L0 and raw pyramid, per-level bytes, time,
          threads used)

prmd decode <file> --level L --x X --y Y --w W --h H -o crop.png [--clamp] [--stats]
        → writes the crop. --stats prints DecodeResult::stats. --clamp uses decode_clamped and prints
          the actual rect.

prmd info <file>
        → header, level table, block counts and per-level bytes
```
Exit codes: 0 ok, 1 usage error, 2 I/O error, 3 format error, 4 out of range.

### 4.2 `prmdbench`
```
prmdbench <image> [--levels 5] [--iters 300] [--seed 1] [--size 4096] [--color gray|rgb|both]
         [--codecs haar,cdf53,laplacian-box,laplacian-gauss,naive,j2k-t256,j2k-t1024]
         [--out <exe dir>/bench/results.md] [--workdir DIR] [--no-entropy-table] [--lossy]
         [--threads 1] [--scaling 1,2,4,8,0]
```
`--threads` is `EncodeOptions::threads` for every encode (default 0 = all hardware
threads; pass `--threads 1` for a single-core encode column comparable across reports). `--scaling` adds an "Encode scaling" table:
each `--codecs` codec encoded at every listed thread count (0 = all hardware threads), median of
3, with the speedup, the threads actually used and a byte-identity check across counts.
Unsuffixed codec names (`haar`, `naive`, …) use the default coder (zstd 1); `+range`, `+raw`, `+zstd19`
(the previous default) variants exist for the entropy table, which compares every coder on haar, cdf53 **and naive**, so the
"vs naive" comparison is always made with the same coder on both sides.

The input is converted to the requested color format(s) and center-cropped or edge-padded to `size²`. All codecs are encoded into a temp directory and then measured (§10). Tables that need a level or crop size the run doesn't have (small `--size`, few `--levels`) print `-` instead of aborting.

### 4.3 `prmdpipeline`
```
prmdpipeline <image> [--levels 6] [--mode haar|cdf53|laplacian|naive] [--down box|gauss]
             [--color gray|rgb] [--quant Q] [--entropy zstd|range|raw] [--entropy-level N]
             [--tile 256] [--cblk 128] [--cache-mib 0] [--runs 5] [--file pipeline.prmd] [--label text]
             [--threads 1]
```
Loads the image, encodes it once, then decodes a fixed navigation plan: one 256² crop per level
from the coarsest down, then 4 pans/jumps at L0 (10 crops at `--levels 6`). Prints median wall
times over `--runs`. Per-step timers (`PRMD_TIMED`, `include/prmd/profile.h`) are compiled in
only with the CMake option `-DPRMD_PROFILE=ON` (default OFF); without it the macro expands to
nothing. `--threads N` (default 0 = all hardware threads) sets the encoder threads; with N > 1 the breakdown adds an
"on workers" column, and steps that ran on pool workers show time summed over threads (they can
exceed their parent's wall time). Results and method: [pipeline_durations.md](pipeline_durations.md).

---

## 5. Level definitions

| Mode | Level L+1 from level L | Non-redundant? | Inverse margin |
|---|---|---|---|
| A haar | `LL = ⌊(l0+l1)/2⌋`, `l0=⌊(a+b)/2⌋`, `l1=⌊(c+d)/2⌋` | yes, 1.0× samples | 0 |
| B cdf53 | 5/3 lifting on the whole level: `d[n]=x[2n+1]−⌊(x[2n]+x[2n+2])/2⌋`, `s[n]=x[2n]+⌊(d[n−1]+d[n]+2)/4⌋`, symmetric extension, rows then cols. The chain is computed on unclamped integers; each *returned* level is clamped to [0, 255] | yes, 1.0× | 1 coefficient |
| C laplacian | D = box `(a+b+c+d+2)>>2` or gauss `[1 4 6 4 1]/16` (reflect-101, `(s+128)>>8`); residual `r = fine − U(coarse)` | no, ~1.33× | 1 sample |
| naive | D = box; every level stored as Base blocks | no, 1.33× raw | — |

**Haar** (mode A), per 2×2 block `a b / c d`:
- rows: `l0=⌊(a+b)/2⌋, h0=a−b`, `l1=⌊(c+d)/2⌋, h1=c−d`
- cols: `LL=⌊(l0+l1)/2⌋, LH=l0−l1`, `HL=⌊(h0+h1)/2⌋, HH=h0−h1`
- inverse of each pair: `x = L + ⌊(H+1)/2⌋`, `y = x − H`

**Upsampler U** (Laplacian), integer and separable. It is phase-matched to D:
- gauss (samples centred on even pixels): `U(2n)=c[n]`, `U(2n+1)=⌊(c[n]+c[n+1]+1)/2⌋`
- box (samples centred between pixels): `U(2n)=⌊(3c[n]+c[n−1]+2)/4⌋`, `U(2n+1)=⌊(3c[n]+c[n+1]+2)/4⌋`
- borders replicate the edge sample

Only D's *output* is stored, so the decoder never runs D. It needs U plus the header's `downsampler` id to pick U's phase.

### 5.1 RGB8
- **Level definition:** every mode's filter is applied **per RGB channel independently**, so an RGB level L equals three gray pyramids stacked. Exactness tests can therefore reuse the gray references channel by channel.
- **Storage:** the encoder computes each channel's base pixels and detail or residual coefficients exactly as for gray, then applies the reversible **YCoCg-R** transform to each co-located triplet (R, G, B → Y, Co, Cg) before entropy coding. The decoder inverts YCoCg-R right after entropy decoding.
- **Why it is lossless:** YCoCg-R is exactly reversible on any integers, so the level definition stays "per-channel RGB" and stays bit-exact, while the shared structure between channels is removed (chroma planes are near zero). Values grow by at most 1 bit, which int32 holds easily.
- **Contexts:** separate context sets for Y and for Co/Cg.
- **Turning it off:** `--no-ycocg` (`EncodeOptions::ycocg = false`) stores R, G and B as they are, at every level and band, and sets bit 1 of header byte 30 so the decoder skips the inverse. It is for measuring what YCoCg-R buys: on the test image, RGB `naive` is 27.9% larger without it. Gray has no color transform, so the option is ignored there and never recorded. `--no-med` (bit 0) works the same way for MED on base blocks; benchmarks in [benchmarks.md](benchmarks.md).

### 5.2 Lossy mode (`quant` = Q > 0)
- **Quantizer:** uniform with rounding, `idx = sign(v)·⌊(|v| + Q/2)/Q⌋`, reconstructed as `idx·Q`, so each value is off by at most Q/2. The indices are what gets entropy-coded; the decoder dequantizes right after entropy decoding. For RGB, quantization happens after YCoCg-R, i.e. on Y/Co/Cg.
- **Steps per band:** LH, HL and Residual use Q. HH uses **2Q**: an HH error reaches the four pixels at ±e/4, versus ±e/2 for LH/HL, so 2Q gives every band the same pixel-domain distortion.
- **The coarsest level stays lossless** (MED). It is about 0.4% of the file, and keeping it exact anchors every coarser-to-finer chain.
- **Haar / cdf53 are open-loop.** The encoder quantizes the bands of the *exact* pyramid. Errors therefore add up toward level 0: level L carries its own quantization error plus everything inherited from levels above. There is no per-pixel bound, only a PSNR.
- **Laplacian is closed-loop.** Going top-down, the encoder rebuilds each level exactly as the decoder will (`U(reconstructed coarse) + dequantized residual`) and takes the next residual against *that*. Quantization errors never propagate, and on gray **every level is within ±Q/2 of the exact pyramid**. This is a hard bound that tests check, and it is useful when a matcher needs a guaranteed tolerance. The price is the redundant 1.33× samples.
- **Naive** (`independent`) is lossless only; the encoder rejects Q > 0.
- Decoding is unchanged apart from the dequantize step, so random access, block counts and byte accounting are identical to lossless.

**cdf53 region inverse.** Fine range `[x0, x1)` uses `n ∈ [⌊x0/2⌋, ⌊(x1−1)/2⌋]` and needs `s[n0 .. n1+1]` and `d[n0−1 .. n1+1]`, clipped with symmetric extension. That is a margin of 1 coefficient per side, per axis, per level.

---

## 6. Decoding algorithm

```
CropDecoder::decode(L, R):               // non-virtual, shared by all codecs
  validate(L, R)                          // §1.3 strict rules
  stats = {}
  img = decode_region(L, R, stats)        // virtual, codec-specific
  return {img, R, stats}

PrmdDecoder::decode_region(L, R, stats):
  out = reconstruct(L, R)                 // updates stats as blocks are fetched/decoded
  return to_u8(out)

reconstruct(L, R):                         // R is in level-L coordinates, inside the level
  if kind(L) == Base:
      return fetch_and_decode_base(L, R)  // blocks overlapping R, MED inverse, copy R
  Rc = clip(T.coarse_needed(R), level L+1)
  Rb = clip(T.band_needed(R),   band plane of L)
  C  = reconstruct(L+1, Rc)               // recursion toward the coarse levels
  B  = fetch_and_decode_detail(L, Rb)     // blocks of level L whose band area overlaps Rb
  return T.inverse(C, B, R)
```
- **Ancestor cost.** Each level up, the region halves in each dimension (plus the margin). After ~log₂(C) levels it fits in one block. So a crop costs its own blocks plus about 1–2 blocks per ancestor level.
- **Block fetch.** The decoder collects the needed block ids for a level, sorts them, merges runs of consecutive ids into one `pread` (tile-major order makes a tile's blocks contiguous), then entropy-decodes each block.
- **Cache (optional).** An LRU of decoded blocks (after MED/YCoCg-R inversion) keyed by block id, with a byte budget. Hits skip both I/O and decoding and are counted in `cache_hits`.
- **Integers end to end.** All levels are reconstructed as int32 and clamped to [0, 255] only when the crop is returned. This matters for Cdf53, whose lowpass can overshoot 0–255 near edges (the 5/3 kernel has negative taps). The coarser level is rebuilt from the *unclamped* values, exactly as the encoder produced them.
- **open()** reads 64 B of header and then the level table and index in one read, and validates them (§3.1). Nothing else is read until `decode`.
- **Naive** (`independent`): every level is Base, so the decoder never recurses.

---

## 7. Entropy coding

The coder is **configurable per file** (`EncodeOptions::entropy`, header byte 28). Every coder implements `internal::BlockCoder`, so the transform, index, I/O and random-access logic don't depend on the choice.

| `Entropy` | What it does | Expected trade-off |
|---|---|---|
| `range` | the context-adaptive binary range coder described below | best ratio; decode is bit-serial (~10 instr/bit) |
| `zstd` (default) | each plane is zigzag-mapped to uint16 and split into a low-byte and a high-byte plane (the high bytes are almost all 0), then all byte planes of the block are compressed as one zstd frame at `entropy_level` (default 1, §7.2) | fast, table-driven decode, mature library; no knowledge of neighbours. Estimated 15–30% larger; **measured +4–5% (gray), +17% (RGB) at level 1 (+12% at level 19)** vs `range` |
| `raw` | zigzag int16, little-endian, no compression | the lower bound on decode cost and the upper bound on size; isolates how much time the entropy stage takes |

- The expected trade-offs are estimates. The benchmark measures them.
- `zstd` and `raw` require every value to fit in int16. All modes satisfy this: details stay within ±1020, and RGB after YCoCg-R stays within ±2040. The encoder checks anyway and throws if a value doesn't fit.
- The zstd frame is created per block with no shared dictionary, which keeps blocks independent. A shared trained dictionary in the header is a possible next step.
- Adding a coder (e.g. rANS) needs a new `Entropy` id, one `BlockCoder` subclass and one line in `make_block_coder`.

### 7.1 Decision record: zstd is the default coder (2026-11-13)

**Decision.** `EncodeOptions::entropy` defaults to `Entropy::Zstd` (level 19; changed to level 1 on 2026-11-16, see §7.2). The CLI defaults to `--entropy zstd`. `range` stays available as an opt-in for size-critical storage. The on-disk ids are unchanged, so files written earlier with `range` still decode.

**Context.** The original default was `range`, chosen on the assumption that a context-adaptive coder was needed for the ratio and that zstd would cost 15–30% in size. The benchmark disproved that assumption. It also showed that the range coder dominates decode time: `raw`, which has no entropy stage, decodes a 256² L0 crop in 0.37 ms, while `range` takes 7.0 ms.

**Evidence** (`prmdbench` gray run: 4096², 5 levels, 300 crops per cell; RGB run: 100 crops per cell; commands in the README):

| | zstd 19 (default at the time) | range | zstd vs range |
|---|---:|---:|---:|
| haar, file size (gray) | 11.59 MiB | 11.13 MiB | +4.1% |
| cdf53, file size (gray) | 11.19 MiB | 10.69 MiB | +4.7% |
| naive, file size (gray) | 15.04 MiB | 14.33 MiB | +5.0% |
| haar, file size (RGB) | 22.92 MiB | 20.52 MiB | +11.7% |
| haar, 256² crop at L0, mean (gray) | 0.56 ms | 7.04 ms | **12.6× faster** |
| haar, 512² crop at L1, mean (gray) | 1.47 ms | 16.20 ms | 11.0× faster |
| haar, 256² crop at L0, mean (RGB) | 2.03 ms | 15.59 ms | 7.7× faster |
| haar, full L0 decode (gray) | 77 ms | 581 ms¹ | 7.6× faster |
| cdf53 lossy q4, size / 256² L0 (gray) | 6.81 MiB / 0.74 ms | 6.29 MiB / 5.35 ms | +8.3% / 7.2× faster |

¹ From the previous run, when range was the default. The current full-decode table covers only the default coder.

**The headline claim still holds under zstd.** The comparison against naive is now made with the *same* coder on both sides, since the benchmark measures naive under every coder: haar is −22.9% vs naive and cdf53 is −25.6% vs naive, both with zstd 19 (at the current default, zstd 1: −23.0% and −25.7%). With range they are −22.3% and −25.3%. The size advantage comes from the pyramid representation, not from the entropy coder.

**Why.**
1. The use case is latency-bound. Visual navigation fetches many small crops on an embedded CPU, and a 12× cut in crop latency (7 ms → 0.55 ms) matters more than 4% of storage. Size remains far below naive.
2. The decoder is simpler and safer on ARM. zstd is a mature, fuzzed, table-driven library that ships in every embedded Linux distro. The range coder is bit-serial custom code, about 10 instructions per bit.
3. Encoding is offline. zstd 19 is slower to encode (gray 1.1 s vs 0.5 s; RGB 8.3 s), but that is paid once per reference image. `--entropy-level 3` gives nearly the same size (+0.1% gray) at a fraction of the encode time.

**Costs accepted.**
- About 4–5% larger gray files and about 12% larger RGB files. zstd cannot exploit the correlation between neighbouring values.
- On RGB, J2K (20.98 MiB) is now smaller than haar+zstd (22.92 MiB). With `range`, haar was smaller (20.52 MiB).

**When to choose `range`.** When storage or transfer size dominates and crops are few or cached. The LRU cache lowers repeated-crop cost to ~0.2 ms in both cases.

**Follow-ups that could close the gap.**
- A zstd dictionary trained per file and stored in the header.
- A table-driven, context-adaptive rANS coder, which could reach the range coder's ratio at near-zstd speed. Adding it needs a new `Entropy` id and one `BlockCoder`.

### 7.2 Decision record: zstd level 1 is the default (2026-11-16)

**Decision.** The default zstd level (`entropy_level` = 0) changes from 19 to 1. Files keep the same format; the level byte in the header stays informational. `--entropy-level 19` remains available for the smallest RGB and lossy files.

**Context.** §7.1 accepted level 19's slow encode because encoding is offline. When reference images are produced or updated on the device, encode time matters too. Measuring every level showed where level 19 actually pays off.

**Evidence: gray, lossless** (4096², 5 levels, `prmd encode`, best of 2 runs). Level 19 makes files no smaller:

| mode | zstd 1 | zstd 3 | zstd 9 | zstd 19 |
|---|---:|---:|---:|---:|
| haar | 83 ms · 11.58 MiB | 96 ms · 11.60 MiB | 217 ms · 11.67 MiB | 1221 ms · 11.59 MiB |
| cdf53 | 330 ms · 11.18 MiB | 354 ms · 11.22 MiB | 483 ms · 11.30 MiB | 1446 ms · 11.19 MiB |
| laplacian-gauss | 297 ms · 14.29 MiB | 314 ms · 14.29 MiB | 485 ms · 14.40 MiB | 1696 ms · 14.29 MiB |
| naive | 106 ms · 15.04 MiB | 124 ms · 15.03 MiB | 283 ms · 15.11 MiB | 1636 ms · 15.04 MiB |

**Evidence: RGB, lossless.** Only level 19 saves anything, about 4% (2.7–4.1% across modes in the later benchmark run):

| mode | zstd 1 | zstd 3 | zstd 9 | zstd 12 | zstd 19 |
|---|---:|---:|---:|---:|---:|
| haar | 296 ms · 23.91 MiB | 350 ms · 24.02 MiB | 1084 ms · 23.79 MiB | 2323 ms · 23.65 MiB | 9191 ms · 22.92 MiB |
| cdf53 | 1047 ms · 24.91 MiB | 1142 ms · 25.24 MiB | 1942 ms · 25.15 MiB | 3026 ms · 25.03 MiB | 9480 ms · 24.24 MiB |

**Evidence: lossy** (gray, bits per pixel; PSNR is unchanged because the coder is lossless). Level 19 saves 3–12%, more at coarser quantization:

| mode | Q | zstd 1 | zstd 19 | zstd 1 vs 19 |
|---|---:|---:|---:|---:|
| haar | 4 / 8 | 3.66 / 2.90 | 3.56 / 2.60 | +3% / +12% |
| cdf53 | 4 / 8 | 3.53 / 2.77 | 3.41 / 2.47 | +4% / +12% |
| laplacian-gauss | 4 / 8 | 4.72 / 3.71 | 4.60 / 3.38 | +3% / +10% |

**Evidence: crop decode.** No change. In the `prmdbench` entropy table, a 256² crop at L0 takes 0.60 ms for haar at both levels, 0.80 ms for cdf53 at both, and 0.59 vs 0.58 ms for naive. (One earlier, shorter comparison showed naive 10–20% slower at level 1; the full run did not reproduce it.)

**Why.** For gray lossless the speed-up is free. For RGB and lossy files, the default trades 3–12% of size for 4–34× faster encoding (mode-dependent: about 4× for cdf53, 15× for haar, 31× for RGB haar), and anyone who wants the smaller file can still ask for it. Why level 19 helps RGB and lossy blocks but not gray lossless ones wasn't investigated; plausibly, quantized and chroma planes contain more long repeated runs that only level 19's search finds.

**Costs accepted.**
- RGB files about 4% larger (haar 23.91 vs 22.92 MiB). The range coder remains the smallest RGB option (20.52 MiB, 1.2 s encode), at 7–12× slower decode.
- Lossy files 3–12% larger, which widens the rate–distortion gap to JPEG2000.

The rest of this section describes the **`range`** coder.

- **Range coder:** LZMA-style binary coder with 32-bit `low`/`range`, byte-wise renormalisation, 12-bit probabilities and adaptation shift 5. It needs no tables, SIMD or FPU, which suits ARM, and the hot loop is about 10 instructions per bit.
- **Value binarisation** of a signed integer v:
  1. `zero` flag
  2. if not zero: a `sign` bit, then `m = |v|−1` coded as exp-Golomb. The prefix (unary, number of bits) uses adaptive contexts, the top 2 suffix bits are adaptive, and the remaining suffix bits are direct.
- **Contexts:** chosen by `bucket(|left| + |up| + (|up-left| + |up-right|)/2)` (neighbours outside the block count as 0), with 8 buckets. Separate model sets per `Band` kind × {luma, chroma}. The second suffix bit is conditioned on the first.
- **Independence:** all probabilities reset to `kProbInit` at the start of each block.

---

## 8. JPEG2000 backend

- **Encode** with OpenJPEG 2.5.4:
  - `numresolution = N`, so `reduce = L` produces level L at `ceil(W/2^L)`
  - 5/3 reversible (lossless), or 9/7 with a PSNR target
  - RGB8: 3 components with MCT on (RCT for lossless, ICT for lossy), which is J2K's own equivalent of YCoCg-R
  - code-blocks `j2k_cblk²`, tiles `j2k_tile²` at full resolution
  - `TLM=YES` writes a tile-length index into the main header
- **Decode:**
  - `open_j2k` reads the main header once and keeps its bytes in RAM. The header length is found by walking marker segments from SOC to the first SOT; `opj_get_cstr_index` returns null after `opj_read_header` in 2.5.4.
  - **Per crop, a fresh codec** (verified, R1): OpenJPEG refuses a second `opj_decode` on the same codec ("Need to decode the main header before…"). The codec re-parses the main header from the RAM copy. That parse cost is part of the measured latency, but its bytes are not re-read from the file.
  - Then `opj_set_decoded_resolution_factor(L)`, `opj_set_decode_area(R·2^L)`, `opj_decode`.
  - I/O goes through a custom `opj_stream` (4 KiB buffer) whose read callback `pread`s the file and counts `bytes_read` and `read_calls`, including OpenJPEG's read-ahead.
  - `tiles_touched` = J2K tiles intersecting the full-resolution window. `blocks_decoded` is reported as tiles (OpenJPEG does not expose code-block counts).
- **Findings from probing (R2):**
  - With TLM and 256² tiles, an L0 crop reads only its tiles: a 100² crop read 59 KB of an 880 KB test file.
  - **At coarse levels J2K reads whole full-resolution tiles.** A tile's codestream holds all resolutions, so a level-3 crop of a 1024² image read the entire file. Splitting tiles into per-resolution tile-parts (`tp_flag='R'`, RPCL) made it *worse*, because OpenJPEG still reads every tile-part and seeks more. The default (LRCP, one tile-part per tile) is kept.
  - **Resolution-progressive layouts don't help with OpenJPEG 2.5.4 (measured 2026-11-16).** The codestream syntax would allow coarse-level access: with tile-parts split by resolution (plus TLM), or a single tile with precincts and PLT packet lengths, a decoder could read only the first tile-parts or packets. OpenJPEG reads all tile-parts of every tile it decodes and doesn't use PLT to skip packets. Mean bytes read per random 256² crop on the 4096² gray benchmark image (lossless 5/3, 5 resolutions, 64² code-blocks, TLM on; 20 crops per level; scratch program, not part of the benchmark):

    | layout | file | L0 | L2 | L3 | L4 (whole level) |
    |---|---:|---:|---:|---:|---:|
    | 256² tiles, LRCP, 1 tile-part (benchmarked) | 10.84 MiB | 194 KiB | 1.15 MiB | 3.8 MiB | 10.8 MiB |
    | 256² tiles, RPCL, tile-parts by resolution | 10.86 MiB | 1.28 MiB | 2.8 MiB | 6.4 MiB | 12.8 MiB |
    | 1024² tiles, RPCL, tile-parts by resolution | 10.82 MiB | 1.26 MiB | 3.0 MiB | 6.5 MiB | 11.1 MiB |
    | untiled, RPCL or RLCP, tile-parts by resolution | 10.82 MiB | 10.8 MiB | 10.8 MiB | 10.8 MiB | 10.8 MiB |
    | untiled, 256² precincts, PLT (RPCL, PCRL, ± tile-parts by resolution) | 10.82 MiB | 10.8 MiB | 10.8 MiB | 10.8 MiB | 10.8 MiB |

    Crop times followed the bytes (untiled: 14–19 ms at L0–L3). So the benchmarked layout is already OpenJPEG's best; better coarse-level granularity would need a decoder that seeks by TLM/PLT per resolution (Kakadu, or a JPIP-style packet fetcher), which was not tried.
- **Levels** are J2K's own LL bands. Tests compare against a full decode at `reduce=L`. The benchmark reports PSNR against the 2×2-mean pyramid.

---

## 9. Testing (gtest, `tests/`)

847 tests, all registered with ctest through `gtest_discover_tests` (`prmd_tests`, plus
`prmd_profile_tests`, which links a copy of the core built with `PRMD_PROFILE`). The suite runs
clean under ASan + UBSan (`-DPRMD_SANITIZE=address,undefined`) and TSan
(`-DPRMD_SANITIZE=thread`; use clang: GCC 15's libtsan crashes in `pthread_cond_wait` on
aarch64 with pointer authentication). Most tests encode with the default `threads = 0`, so on a
multi-core machine the crop matrix, lossy and corruption tests run on multithreaded output;
`test_threads.cpp` covers thread counts explicitly.

| Test file | What it covers |
|---|---|
| `test_image.cpp` | `Rect`/`intersect` (including near `INT_MAX`), `ColorFormat` table, `Image::crop`, `convert` RGB↔Gray, PNG round-trip (gray/RGB), JPEG load (gray/RGB, written with libjpeg in the test), I/O errors (missing, garbage, truncated JPEG) |
| `test_rangecoder.cpp` | round-trip of random, skewed and all-zero bit streams; `encode_direct` at every width; carry-propagation stress; skewed data compresses near entropy; truncated input sets `overrun()` without crashing |
| `test_transform.cpp` | `build_pyramid` equals the independent `tests/reference.cpp` for every mode × gray/RGB × odd sizes (1×9, 2×3, 101×77, 257×130); forward/inverse identity for each transform incl. values outside 0–255; region inverse equals full inverse **and depends only on `*_needed` samples** (everything else overwritten with garbage) |
| `test_blockcoder.cpp` | parameterised over every `Entropy`: round-trip of random, Laplacian-distributed and extreme values for every `Band` and 1/3 channels; truncated payload → `FormatError`; full int32 magnitudes for range; out-of-int16 → zstd/raw throw; YCoCg-R and MED round-trip |
| `test_codec.cpp` | **crop matrix** for 11 .prmd configs (haar, cdf53, laplacian-box/gauss, naive, haar+range, cdf53+raw, naive without MED, naive without MED and YCoCg-R, haar+range without both, laplacian-box without YCoCg-R) × gray (1024², 1000×777) and RGB (512×384, 301×517) × all 5 levels: inside one block, inside one tile, across 2 tiles h/v, across 4 tiles, 4 edges, 4 corners, full level, 1×1. Plus a seeded random-crop fuzz test. Every result is an exact compare against the reference pyramid. Also: tiny images (1×1, 3×2, 17×1, 6 levels), non-default tile/cblk, encoder color conversion, **bounds** (strict throws, clamped intersects), **random access** (below), cache, **format** corruption (magic, version, color id, entropy id, flag bits, reserved bytes, huge block count, huge dimensions, too-small cblk, level table, truncation) and option validation (including geometry the level table can't hold); bounds with coordinates near `INT_MAX`; haar and cdf53 smaller than naive; the MED/YCoCg-R flags recorded in byte 30 (and not recorded for gray), and MED making naive smaller; **LRU eviction** (a 300 KiB budget, far below the working set, over 250 random crops at every level: all exact, and an evicted crop is decoded again); **corrupt payloads** with a valid index, for every config and so every coder: random bytes in every block (gray and RGB) never crash and zstd rejects every level, and blocks cut by 1 byte, half or all of their payload give `FormatError` for zstd and raw (range: no crash). These run clean under ASan + UBSan |
| `test_lossy.cpp` | haar, cdf53, laplacian-box/gauss and haar+range × gray/RGB × Q ∈ {2, 8}: (1) every decoded level equals an **independent whole-plane simulation** of §5.2, built in the test from the exact pyramid without the codec's block, format or entropy code; (2) 150 random crops equal the crop of the full-level decode (seamless, consistent); (3) the coarsest level is exact, PSNR thresholds hold, and size decreases with Q and stays below lossless; (4) Laplacian on gray: max \|error\| ≤ Q/2 at every level for Q ∈ {2, 5, 8, 16}; quantizer bound check |
| `test_cli.cpp` | the `prmd` executable as a child process: exit codes 0 (encode, decode, info, help, J2K encode), 1 (usage: missing or unknown command and options, bad choices, invalid numbers such as `--x ""`, `12abc`, `4294967296`, `--j2k-lossy-psnr 1e999`), 2 (missing input, unwritable output), 3 (bad header, truncated data), 4 (out of range, including `--x 2147483647` and `--w 2147483647`, bad level, empty rect); the written PNG equals the reference crop; `--clamp` writes the intersection; `--no-med --no-ycocg` encodes, decodes exactly and is rejected for J2K; `--threads` rejects `-1`, `abc`, `1.5`, a missing value, and writes the `--threads 1` file for 0, 2, 3, 16, 1000 |
| `test_j2k.cpp` | gray/RGB × 256/1024 tiles: L0 lossless vs source; 150 random crops + edges/corners at every level vs J2K's own full-level decode; TLM single-tile crop reads < ¼ of the file; bounds |
| `test_parallel.cpp` | the thread pool: `resolve_threads` (0, hardware 0, negative, clamping to 256); every index of [0, n) exactly once for 1–16 threads and several grains; inline single-call execution for 1 thread and small jobs; zero and one item; more threads than items; results identical for every thread count; the lowest failing index's exception under random delays and inside chunks; reuse across 300 runs and after exceptions; nested `run`; row stripes |
| `test_threads.cpp` | the multithreaded encoder over a 265-case matrix (`thread_matrix.h`: 12 modes incl. both downsamplers, all coders, no-MED/no-YCoCg × gray/RGB × 1×1, 3×2, 17×1, 1×23, single-block, many-block, odd and 1000×777 sizes; lossy steps 1/3/8; zstd 3/19/22; 1 and 8 levels; cblk 16 and 4096; encoder colour conversion; full-range noise; integer-only images): **golden hashes** of the serial encoder before threading (fc0a629) and **identical files** at 2, 3, 7, 16, 0 and 1000 threads; `threads = 1` codes every block on the calling thread and starts no thread (`/proc/self/task`), 4 threads start exactly 3 and join them; `EncodeReport::threads`, clamping to blocks and 256, negative counts rejected without a file; parallel `forward`, downsamplers, Laplacian upsampler and fused `to_planes` equal the serial ones; multithreaded files decode to the reference pyramid; four user threads encoding at once; **fault injection** (a coder that throws for some blocks): same exception at every thread count, no file left, an existing file untouched, early stop; J2K files identical across thread counts and its per-tile thread cap |
| `test_profile.cpp` | (`prmd_profile_tests`) worker `PRMD_TIMED` scopes merged under the caller's scope with complete call counts, `worker_calls`/`worker_ms`, wall vs summed time, scopes opened only on workers, reset; a 4-thread encode has the same tree and call counts as a serial one |

**Random-access proof** (`test_codec.cpp`, every .prmd config):
- (a) An independent re-derivation of the dependency rule gives the expected block set. `blocks_decoded` must equal its size, and `bytes_read` must equal the exact sum of those payloads.
- (b) A copy of the file has every payload *outside* that set overwritten with random bytes. The crop must still be exact.
- (c) Negative control: corrupting one block *inside* the set must change the crop or fail. Without this, (b) could pass vacuously.

---

## 10. Benchmark methodology (`tools/prmd_bench.cpp`)

- **Input:** the user's JPEG (`test_image.jpg`, 8824×5336 RGB aerial photo), center-cropped to 4096², N = 5. Gray8 is the primary run (default `--out`, `build/bench/results.md`). An RGB8 run uses 100 iterations, no entropy table and no `j2k-t1024`, but adds `haar+range`, `haar+zstd19` and the lossy table (`--out build/bench/results_rgb.md`). Both reports are regenerated output in the git-ignored build directory.
- **Entropy coders:** for `haar`, `cdf53` and `naive`, one table comparing `zstd` 1 (default), `zstd` 19 (the previous default), `range` and `raw`: file size, size vs the default, size vs naive *under the same coder*, mean and p95 latency for 256² crops at L0 and 512² at L1. `raw` approximates the non-entropy share of decode time. All other tables use the default coder (zstd 1).
- **Sizes:** each codec's file size against raw L0 (16 MiB gray / 48 MiB RGB) and the raw pyramid (≈21.3 / 64 MiB), plus per-level bytes. PNG-per-256-tile is included as a size-only reference.
- **Latency:**
  - crop sizes 256² and 512², at every level whose dimensions are ≥ the crop
  - `--iters` random positions per row (300 by default and in the published numbers), uniform and non-aligned, with a fixed seed
  - for each codec: mean, p50 and p95 per crop (`steady_clock`), plus mean `tiles_touched`, `blocks_decoded` and `bytes_read`
  - block cache off, after 10 warm-up crops (the file itself is in the OS page cache)
  - the file is in the page cache, so `bytes_read` is the I/O metric
- **Trajectory:** a 1000-step random walk of 256² crops at L0 with the cache on, for the `.prmd` modes.
- **Reference rows:** full-level decode of L0–L2 (what a crop costs without random access).
- **Lossy** (`--lossy`): `.prmd` haar, cdf53 and laplacian-gauss at quant 2/4/8 with the default coder (plus cdf53+range at quant 4) against J2K 9/7 at PSNR targets 50/45/40/35 dB. Reports size, bits/px, PSNR at L0 vs the source, PSNR at L2 vs the codec's own lossless L2 (n/a for J2K, whose 9/7 levels differ from its 5/3 levels), max |error| at L0, and L0 crop latency.
- **Level quality:** PSNR of each mode's levels against the `haar` 2×2-mean pyramid. One sample PNG of level 3 (centre 256²) is saved per *level definition* (`haar`, `box`, `gauss`, `cdf53`, and each J2K tiling) for visual aliasing comparison. Codecs that share a definition decode identical levels. J2K tilings count separately, because J2K transforms each tile with symmetric extension at tile borders.
- **Output:** markdown tables on stdout and in the `--out` path (default `<exe dir>/bench/results.md`: `bench/` in the directory of the `prmdbench` executable, resolved at run time, so `build/bench/results.md` for `build/prmdbench`), plus the build flags and CPU model. The level-3 samples go to `samples/` next to the report, so `build/bench/samples/` by default.

---

## 11. Risks, limitations and open questions

| # | Item | Mitigation / plan |
|---|---|---|
| R1 | **Confirmed:** OpenJPEG decodes once per codec instance | a fresh codec per crop; the main header is served from RAM (parse cost included in latency) |
| R2 | **Confirmed in part:** TLM skips other tiles at L0, but coarse levels read whole full-resolution tiles | reported as a finding (§8); the `j2k-t256` vs `t1024` rows show the effect. Resolution-progressive layouts (tile-parts by resolution, precincts + PLT) were measured and don't help with OpenJPEG 2.5.4 (§8) |
| R3 | ancestor decoding makes a single-block crop cost about N blocks in A/B/C | geometric shrink, ~1–2 blocks per level; LRU cache; quantified in the benchmark |
| R4 | resetting contexts per block costs compression | `cblk` is a parameter; benchmark 64/128/256 |
| L1 | RGB handles 8-bit 3-channel only; YCoCg-R is applied to coefficients, not as a pre-transform | next step: a context that shares statistics across channels; other formats (alpha, 16-bit) |
| L2 | a single-threaded decoder (the encoder is multithreaded, §1.4) | next step: the encoder's `Pool` (§2.8) can drive decode. In `PrmdDecoder::read_run` the blocks missing from the cache for one level are independent decodes: decode them into a vector in parallel, then insert into the cache and the output serially in the same order (the cache stays single-threaded; lowest-index exception rule for corrupt payloads). Full-level decodes and large crops can also use row stripes in `inverse`. A 256² crop (≈20 blocks, 0.6 ms) gains little, so parallelize only above a block threshold; `DecoderOptions::threads` should default to 1, because latency-bound callers usually parallelize across crops |
| L3 | the box filter aliases (modes A and naive) | modes B and C-gauss exist for exactly this reason; quality is reported |

---

## 12. Source layout

```
CMakeLists.txt                 Release: -O2 -mcpu=native; _FILE_OFFSET_BITS=64; options PRMD_PROFILE (OFF),
                               PRMD_SANITIZE (address,undefined | thread). Deps: JPEG, PNG, GTest, zstd,
                               libopenjp2 (pkg-config), Threads
include/prmd/                  image.h image_io.h crop_decoder.h encoder.h pyramid.h errors.h profile.h
include/prmd/internal/         region.h planes.h transform.h rangecoder.h (header-only) blockcoder.h
                               prmd_format.h prmd_codec.h j2k_codec.h file.h (pread wrapper)
                               timing.h (Clock) parallel.h (thread pool) profile_threads.h
src/                           image.cpp image_io.cpp pyramid.cpp transform.cpp (haar, cdf53, laplacian,
                               downsamplers) blockcoder.cpp (factory, YCoCg-R, MED) blockcoder_range.cpp
                               blockcoder_bytes.cpp (zstd, raw) prmd_format.cpp prmd_encoder.cpp
                               prmd_decoder.cpp j2k_codec.cpp crop_decoder.cpp encoder.cpp profile.cpp
                               parallel.cpp
tools/prmd_cli.cpp             → executable `prmd` (library target: `prmd_core`, to avoid a name clash)
tools/prmd_pipeline.cpp        → executable `prmdpipeline` (§4.3)
tools/prmd_bench.cpp           → executable `prmdbench` (§4.2, §10)
tests/                         reference.{h,cpp} test_util.h thread_matrix.h test_image.cpp
                               test_rangecoder.cpp test_transform.cpp test_blockcoder.cpp test_codec.cpp
                               test_lossy.cpp test_j2k.cpp test_cli.cpp test_parallel.cpp
                               test_threads.cpp test_profile.cpp
build/bench/                   benchmark output, git-ignored: reports results*.md, level-3 samples in samples/
docs/design.md                 this document
docs/writeup.md                results write-up (1–2 pages)
docs/benchmarks.md             full benchmark tables: every mode, lossy, RGB
docs/modes.md                  modes and options with worked examples
docs/benchmarks-pi5.md         the benchmark on a Raspberry Pi 5
docs/pipeline_durations.md     per-step encode/decode timings (prmdpipeline)
docs/img/                      level samples used by modes.md
```
