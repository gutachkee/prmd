// prmdbench — size / latency / I/O / quality benchmark for every codec
// (DESIGN.md §10).
//
// usage: prmdbench <image> [--levels 5] [--iters 300] [--seed 1] [--size 4096]
//                  [--color gray|rgb|both] [--codecs a,b,...]
//                  [--out <build dir>/bench/results.md] [--workdir DIR]
//                  [--no-entropy-table] [--lossy]
//
// Everything the run produces goes next to --out: the report itself and the
// level-3 sample PNGs in <dir of --out>/samples/. The default --out is in the
// CMake build directory (PRMD_BENCH_OUT_DIR), which git ignores.

#include <unistd.h>

#ifndef PRMD_BENCH_OUT_DIR
#define PRMD_BENCH_OUT_DIR "bench_out"  // built outside CMake
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/image_io.h"
#include "prmd/internal/timing.h"
#include "prmd/pyramid.h"

using namespace prmd;
using prmd::internal::Clock;
using prmd::internal::elapsed_ms;
namespace fs = std::filesystem;

namespace {

// ----------------------------------------------------------------------------
// Codec catalogue

struct CodecSpec {
  std::string name;
  Codec codec;
  EncodeOptions opt;
};

EncodeOptions make_prmd_options(int levels,
                                ColorFormat color,
                                Transform t,
                                Downsampler d,
                                Entropy e,
                                int entropy_level) {
  EncodeOptions o;
  o.levels = levels;
  o.color = color;
  o.transform = t;
  o.downsampler = d;
  o.entropy = e;
  o.entropy_level = entropy_level;
  return o;
}

EncodeOptions make_j2k_options(int levels, ColorFormat color, int tile) {
  EncodeOptions o;
  o.levels = levels;
  o.color = color;
  o.j2k_tile = tile;
  return o;
}

std::vector<CodecSpec> all_specs(int levels, ColorFormat color) {
  auto prmd = [&](const char* n, Transform t, Downsampler d,
                  Entropy e = Entropy::Zstd, int lvl = 0) {
    return CodecSpec{n, Codec::Prmd,
                     make_prmd_options(levels, color, t, d, e, lvl)};
  };
  auto j2k = [&](const char* n, int tile) {
    return CodecSpec{n, Codec::J2K, make_j2k_options(levels, color, tile)};
  };

  constexpr Transform kHaar = Transform::Haar;
  constexpr Transform kCdf53 = Transform::Cdf53;
  constexpr Transform kNaive = Transform::Independent;
  constexpr Downsampler kBox = Downsampler::Box;
  return {
      prmd("haar", kHaar, kBox),
      prmd("cdf53", kCdf53, kBox),
      prmd("laplacian-box", Transform::Laplacian, kBox),
      prmd("laplacian-gauss", Transform::Laplacian, Downsampler::Gauss),
      prmd("naive", kNaive, kBox),
      j2k("j2k-t256", 256),
      j2k("j2k-t1024", 1024),
      // Unsuffixed names use the default coder (zstd, level 1); +zstd19 is
      // the previous default, kept for comparison.
      prmd("haar+zstd19", kHaar, kBox, Entropy::Zstd, 19),
      prmd("haar+range", kHaar, kBox, Entropy::Range),
      prmd("haar+raw", kHaar, kBox, Entropy::Raw),
      prmd("cdf53+zstd19", kCdf53, kBox, Entropy::Zstd, 19),
      prmd("cdf53+range", kCdf53, kBox, Entropy::Range),
      prmd("cdf53+raw", kCdf53, kBox, Entropy::Raw),
      prmd("naive+zstd19", kNaive, kBox, Entropy::Zstd, 19),
      prmd("naive+range", kNaive, kBox, Entropy::Range),
      prmd("naive+raw", kNaive, kBox, Entropy::Raw),
  };
}

const std::vector<std::string> kMainCodecs = {
    "haar",  "cdf53",    "laplacian-box", "laplacian-gauss",
    "naive", "j2k-t256", "j2k-t1024"};

const std::vector<std::string> kEntropyCodecs = {
    "haar",  "haar+zstd19",  "haar+range",  "haar+raw",
    "cdf53", "cdf53+zstd19", "cdf53+range", "cdf53+raw",
    "naive", "naive+zstd19", "naive+range", "naive+raw"};

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

bool is_j2k(const std::string& name) {
  return name.rfind("j2k", 0) == 0;
}

// ----------------------------------------------------------------------------
// Image and number helpers

// Center-crop (or edge-pad) to size × size.
Image fit_square(const Image& src, int size) {
  Image out(size, size, src.format);
  const int channels = src.channels();
  const int ox = (src.width - size) / 2;
  const int oy = (src.height - size) / 2;

  for (int y = 0; y < size; ++y) {
    const int sy = std::clamp(y + oy, 0, src.height - 1);
    for (int x = 0; x < size; ++x) {
      const int sx = std::clamp(x + ox, 0, src.width - 1);
      for (int c = 0; c < channels; ++c) {
        out.row(y)[size_t(x) * size_t(channels) + size_t(c)] =
            src.at(sx, sy, c);
      }
    }
  }
  return out;
}

double psnr(const Image& a, const Image& b) {
  double se = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    const double d = double(a.pixels[i]) - double(b.pixels[i]);
    se += d * d;
  }
  if (se == 0) {
    return INFINITY;
  }
  return 10.0 * std::log10(255.0 * 255.0 * double(a.pixels.size()) / se);
}

int max_abs_error(const Image& a, const Image& b) {
  int m = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    m = std::max(m, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
  }
  return m;
}

double mean(const std::vector<double>& v) {
  double sum = 0;
  for (double x : v) {
    sum += x;
  }
  return sum / double(v.size());
}

// 95th percentile of an ascending-sorted, non-empty vector.
double p95_of_sorted(const std::vector<double>& sorted) {
  const size_t n = sorted.size();
  return sorted[std::min(n - 1, size_t(std::ceil(0.95 * double(n))) - 1)];
}

std::string fmt(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof b, f, v);
  return b;
}

std::string ms(double v) {
  return fmt("%.2f ms", v);
}

std::string bytes_h(double b) {
  if (b >= 1 << 20) {
    return fmt("%.2f MiB", b / double(1 << 20));
  }
  if (b >= 1 << 10) {
    return fmt("%.1f KiB", b / 1024.0);
  }
  return fmt("%.0f B", b);
}

std::string psnr_s(double p) {
  return std::isinf(p) ? "∞ (exact)" : fmt("%.2f dB", p);
}

std::string percent_change(double value, double reference) {
  return fmt("%+.1f%%", 100.0 * (value / reference - 1));
}

std::string level_name(int level) {
  return "L" + std::to_string(level);
}

// Markdown table: first column left-aligned, the rest right-aligned.
struct Table {
  std::vector<std::string> head;
  std::vector<std::vector<std::string>> rows;

  std::string str() const {
    std::ostringstream o;
    o << "|";
    for (const auto& h : head) {
      o << " " << h << " |";
    }
    o << "\n|";
    for (size_t i = 0; i < head.size(); ++i) {
      o << (i == 0 ? "---|" : "---:|");
    }
    o << "\n";
    for (const auto& r : rows) {
      o << "|";
      for (const auto& c : r) {
        o << " " << c << " |";
      }
      o << "\n";
    }
    return o.str();
  }
};

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  std::string model;
  std::string impl;
  std::string part;
  while (std::getline(f, line)) {
    auto value = [&] { return line.substr(line.find(':') + 2); };
    if (line.rfind("model name", 0) == 0 && model.empty()) {
      model = value();
    }
    if (line.rfind("CPU implementer", 0) == 0 && impl.empty()) {
      impl = value();
    }
    if (line.rfind("CPU part", 0) == 0 && part.empty()) {
      part = value();
    }
  }
  if (!model.empty()) {
    return model;
  }
  return "ARM implementer " + impl + " part " + part;
}

// ----------------------------------------------------------------------------
// Command line

struct Args {
  std::string image;
  std::string out = PRMD_BENCH_OUT_DIR "/results.md";
  std::string workdir;
  std::string color = "gray";
  int levels = 5;
  int iters = 300;
  int size = 4096;
  unsigned seed = 1;
  std::vector<std::string> codecs;
  bool entropy_table = true;
  bool lossy = false;
};

[[noreturn]] void die(const char* format, const std::string& arg) {
  std::fprintf(stderr, format, arg.c_str());
  std::exit(1);
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        die("missing value for %s\n", s);
      }
      return argv[++i];
    };

    if (s == "--levels") {
      a.levels = std::stoi(next());
    } else if (s == "--iters") {
      a.iters = std::stoi(next());
    } else if (s == "--seed") {
      a.seed = unsigned(std::stoul(next()));
    } else if (s == "--size") {
      a.size = std::stoi(next());
    } else if (s == "--color") {
      a.color = next();
    } else if (s == "--out") {
      a.out = next();
    } else if (s == "--workdir") {
      a.workdir = next();
    } else if (s == "--no-entropy-table") {
      a.entropy_table = false;
    } else if (s == "--lossy") {
      a.lossy = true;
    } else if (s == "--codecs") {
      std::stringstream ss(next());
      std::string c;
      while (std::getline(ss, c, ',')) {
        a.codecs.push_back(c);
      }
    } else if (s.rfind("--", 0) == 0) {
      die("unknown option %s\n", s);
    } else {
      a.image = s;
    }
  }

  if (a.image.empty()) {
    std::fprintf(stderr,
                 "usage: prmdbench <image> [--levels 5] [--iters 300] [--seed "
                 "1] [--size 4096]\n"
                 "                 [--color gray|rgb|both] [--codecs a,b,...] "
                 "[--out " PRMD_BENCH_OUT_DIR "/results.md]\n"
                 "                 [--workdir DIR] [--no-entropy-table] "
                 "[--lossy]\n");
    std::exit(1);
  }
  if (a.codecs.empty()) {
    a.codecs = kMainCodecs;
  }
  return a;
}

// ----------------------------------------------------------------------------
// Measurements

struct LatencyRow {
  double mean_ms = 0;
  double p50 = 0;
  double p95 = 0;
  double tiles = 0;
  double blocks = 0;
  double bytes = 0;
  double reads = 0;
};

// `iters` random crop × crop decodes at `level` (after 10 warm-up decodes).
LatencyRow measure(CropDecoder& dec,
                   int level,
                   int crop,
                   int iters,
                   unsigned seed) {
  const LevelInfo li = dec.level_info(level);
  std::mt19937 rng(seed * 7919u + unsigned(level) * 131u + unsigned(crop));
  auto rand_rect = [&] {
    const int x = int(rng() % unsigned(li.width - crop + 1));
    const int y = int(rng() % unsigned(li.height - crop + 1));
    return Rect{x, y, crop, crop};
  };

  for (int i = 0; i < 10; ++i) {
    (void)dec.decode(level, rand_rect());  // warm-up
  }

  std::vector<double> times;
  LatencyRow r;
  for (int i = 0; i < iters; ++i) {
    const Rect rect = rand_rect();
    const auto start = Clock::now();
    const DecodeResult res = dec.decode(level, rect);
    times.push_back(elapsed_ms(start));
    r.tiles += res.stats.tiles_touched;
    r.blocks += res.stats.blocks_decoded;
    r.bytes += double(res.stats.bytes_read);
    r.reads += res.stats.read_calls;
  }

  std::sort(times.begin(), times.end());
  r.mean_ms = mean(times);
  r.p50 = times[size_t(iters / 2)];
  r.p95 = p95_of_sorted(times);
  r.tiles /= iters;
  r.blocks /= iters;
  r.bytes /= iters;
  r.reads /= iters;
  return r;
}

// Everything one color run shares across its report sections.
struct Run {
  Run(const Args& a, ColorFormat c, const Image& source)
      : args(a),
        color(c),
        cname(info(c).name),
        img(fit_square(convert(source, c), a.size)),
        work(a.workdir.empty() ? fs::temp_directory_path() /
                                     ("prmdbench_" + std::to_string(::getpid()))
                               : fs::path(a.workdir)),
        channels(img.channels()),
        raw0(double(a.size) * a.size * channels) {
    for (int level = 0, s = a.size; level < a.levels;
         ++level, s = (s + 1) / 2) {
      raw_pyramid += double(s) * s * channels;
    }
  }

  const Args& args;
  ColorFormat color;
  std::string cname;
  Image img;
  fs::path work;
  int channels = 0;
  double raw0 = 0;         // raw bytes of level 0
  double raw_pyramid = 0;  // raw bytes of all levels
  std::map<std::string, std::string> path;
  std::map<std::string, EncodeReport> rep;
  std::map<std::string, std::unique_ptr<CropDecoder>> dec;

  // Bits per level-0 pixel for a file of `bytes`.
  double bits_per_px(double bytes) const {
    return 8.0 * bytes / (double(args.size) * args.size);
  }
};

void encode_all(Run& run) {
  std::vector<std::string> wanted = run.args.codecs;
  if (run.args.entropy_table) {
    for (const auto& n : kEntropyCodecs) {
      if (!contains(wanted, n)) {
        wanted.push_back(n);
      }
    }
  }

  for (const auto& spec : all_specs(run.args.levels, run.color)) {
    if (!contains(wanted, spec.name)) {
      continue;
    }
    const std::string ext = spec.codec == Codec::J2K ? ".j2k" : ".prmd";
    const std::string p =
        (run.work / (spec.name + "_" + run.cname + ext)).string();
    std::fprintf(stderr, "[%s] encoding %s...\n", run.cname.c_str(),
                 spec.name.c_str());
    run.rep[spec.name] = encode_file(run.img, p, spec.codec, spec.opt);
    run.path[spec.name] = p;
  }
}

// PNG per 256-tile of the naive (box) pyramid — size-only reference.
double png_tiles_bytes(const Run& run) {
  const auto pyr = build_pyramid(run.img, run.args.levels,
                                 Transform::Independent, Downsampler::Box);
  const fs::path tile_path = run.work / "tile.png";
  double bytes = 0;
  for (const auto& level : pyr) {
    for (int y = 0; y < level.height; y += 256) {
      for (int x = 0; x < level.width; x += 256) {
        save_png(tile_path.string(),
                 level.crop(intersect({x, y, 256, 256}, level.bounds())));
        bytes += double(fs::file_size(tile_path));
      }
    }
  }
  fs::remove(tile_path);
  return bytes;
}

void file_size_table(Run& run, std::ostringstream& md) {
  const double naive_bytes =
      run.rep.count("naive") ? double(run.rep["naive"].file_bytes) : 0;
  const double png_bytes = png_tiles_bytes(run);

  Table sizes{{"codec", "file size", "bits/px", "vs raw L0", "vs raw pyramid",
               "vs naive", "encode"},
              {}};
  auto size_row = [&](const std::string& n, double b, double encode_ms) {
    sizes.rows.push_back(
        {n, bytes_h(b), fmt("%.3f", run.bits_per_px(b)),
         fmt("%.3f×", run.raw0 / b), fmt("%.3f×", run.raw_pyramid / b),
         naive_bytes > 0 ? percent_change(b, naive_bytes) : "-",
         encode_ms > 0 ? fmt("%.0f ms", encode_ms) : "-"});
  };

  for (const auto& n : run.args.codecs) {
    if (run.rep.count(n)) {
      size_row(n, double(run.rep[n].file_bytes), run.rep[n].encode_ms);
    }
  }
  size_row("png-tiles (naive pyramid)", png_bytes, 0);
  sizes.rows.push_back({"raw level 0", bytes_h(run.raw0),
                        fmt("%.3f", 8.0 * run.channels), "1.000×", "-", "-",
                        "-"});
  sizes.rows.push_back(
      {"raw pyramid", bytes_h(run.raw_pyramid),
       fmt("%.3f", 8.0 * run.raw_pyramid / run.raw0 * run.channels), "-",
       "1.000×", "-", "-"});

  md << "### File size (lossless; bits/px over level-0 pixels)\n\n"
     << sizes.str() << "\n";
}

void level_bytes_table(Run& run, std::ostringstream& md) {
  Table t{{"codec"}, {}};
  for (int level = 0; level < run.args.levels; ++level) {
    t.head.push_back(level_name(level));
  }

  for (const auto& n : run.args.codecs) {
    if (!run.rep.count(n) || is_j2k(n)) {
      continue;
    }
    std::vector<std::string> row{n};
    for (int level = 0; level < run.args.levels; ++level) {
      row.push_back(bytes_h(double(run.rep[n].level_bytes[size_t(level)])));
    }
    t.rows.push_back(row);
  }
  md << "### Payload bytes per level (.prmd)\n\n" << t.str() << "\n";
}

void latency_tables(Run& run, std::ostringstream& md) {
  for (int crop : {256, 512}) {
    Table t{{"codec", "level", "mean", "p50", "p95", "tiles touched",
             "blocks decoded", "bytes read", "read calls"},
            {}};
    for (const auto& n : run.args.codecs) {
      if (!run.dec.count(n)) {
        continue;
      }
      for (int level = 0; level < run.args.levels; ++level) {
        const LevelInfo li = run.dec[n]->level_info(level);
        if (li.width < crop || li.height < crop) {
          continue;
        }
        std::fprintf(stderr, "[%s] %s L%d %d² ...\n", run.cname.c_str(),
                     n.c_str(), level, crop);
        const auto r =
            measure(*run.dec[n], level, crop, run.args.iters, run.args.seed);
        t.rows.push_back({n, level_name(level), ms(r.mean_ms), ms(r.p50),
                          ms(r.p95), fmt("%.1f", r.tiles),
                          fmt("%.1f", r.blocks), bytes_h(r.bytes),
                          fmt("%.1f", r.reads)});
      }
    }
    md << "### Random " << crop << "×" << crop << " crops (" << run.args.iters
       << " per row, cold cache, non-aligned positions)\n\n"
       << t.str() << "\n";
  }
}

void entropy_table(Run& run, std::ostringstream& md) {
  Table t{{"coder", "file size", "vs default (zstd 1)", "vs naive, same coder",
           "mean 256² L0", "p95 256² L0", "mean 512² L1"},
          {}};

  for (const auto& n : kEntropyCodecs) {
    if (!run.dec.count(n)) {
      continue;
    }
    const size_t plus = n.find('+');
    const std::string base = n.substr(0, plus);
    const double bytes = double(run.rep[n].file_bytes);

    // The naive file with the same entropy coder, if there is one.
    const std::string naive =
        "naive" + (plus == std::string::npos ? "" : n.substr(plus));
    const std::string vs_naive =
        (!run.rep.count(naive) || base == "naive")
            ? "-"
            : percent_change(bytes, double(run.rep[naive].file_bytes));

    const auto r0 = measure(*run.dec[n], 0, 256, run.args.iters, run.args.seed);
    const auto r1 = measure(*run.dec[n], 1, 512, run.args.iters, run.args.seed);
    t.rows.push_back({n, bytes_h(bytes),
                      percent_change(bytes, double(run.rep[base].file_bytes)),
                      vs_naive, ms(r0.mean_ms), ms(r0.p95), ms(r1.mean_ms)});
  }
  md << "### Entropy coders (" << run.args.iters << " crops per cell)\n\n"
     << t.str() << "\n";
}

// Navigation-like random walk, with and without the block cache.
void trajectory_table(Run& run, std::ostringstream& md) {
  constexpr int kSteps = 1000;
  const int size = run.args.size;
  Table t{{"codec", "cache", "mean", "p95", "blocks decoded", "cache hits",
           "bytes read"},
          {}};

  for (const auto& n : run.args.codecs) {
    if (!run.path.count(n) || is_j2k(n)) {
      continue;
    }
    for (size_t cache : {size_t(0), size_t(64) << 20}) {
      DecoderOptions o;
      o.cache_bytes = cache;
      auto d = open_decoder(run.path[n], o);
      std::mt19937 rng(run.args.seed);
      int x = size / 2;
      int y = size / 2;
      std::vector<double> times;
      double blocks = 0;
      double hits = 0;
      double bytes = 0;

      for (int i = 0; i < kSteps; ++i) {
        x = std::clamp(x + int(rng() % 65u) - 32, 0, size - 256);
        y = std::clamp(y + int(rng() % 65u) - 32, 0, size - 256);
        const auto start = Clock::now();
        const auto res = d->decode(0, {x, y, 256, 256});
        times.push_back(elapsed_ms(start));
        blocks += res.stats.blocks_decoded;
        hits += res.stats.cache_hits;
        bytes += double(res.stats.bytes_read);
      }

      const double mean_ms = mean(times);
      std::sort(times.begin(), times.end());
      t.rows.push_back({n, cache ? "64 MiB" : "off", ms(mean_ms),
                        ms(p95_of_sorted(times)), fmt("%.1f", blocks / kSteps),
                        fmt("%.1f", hits / kSteps), bytes_h(bytes / kSteps)});
    }
  }
  md << "### Trajectory: 1000-step random walk of 256² crops at L0 (±32 "
        "px/step)\n\n"
     << t.str() << "\n";
}

// The "decode everything, then crop" baseline.
void full_decode_table(Run& run, std::ostringstream& md) {
  Table t{{"codec", "full L0 decode", "full L1 decode", "full L2 decode"}, {}};
  for (const auto& n : run.args.codecs) {
    if (!run.dec.count(n)) {
      continue;
    }
    std::vector<std::string> row{n};
    for (int level = 0; level < 3; ++level) {
      const LevelInfo li = run.dec[n]->level_info(level);
      const auto start = Clock::now();
      (void)run.dec[n]->decode(level, {0, 0, li.width, li.height});
      row.push_back(fmt("%.1f ms", elapsed_ms(start)));
    }
    t.rows.push_back(row);
  }
  md << "### Full-level decode (what a crop costs without random access)\n\n"
     << t.str() << "\n";
}

// Name of the level definition a codec produces, i.e. which images its levels
// are. Codecs with the same definition decode identical levels (the entropy
// coder never changes pixels), so one sample PNG per definition is enough.
std::string level_definition(const CodecSpec& spec) {
  if (spec.codec == Codec::J2K) {
    // J2K runs the 5/3 transform per tile with symmetric extension at tile
    // borders, so its levels differ from whole-image cdf53 near tile edges
    // and depend on the tile size: each tiling is its own definition.
    return spec.name;
  }
  switch (spec.opt.transform) {
    case Transform::Haar:
      return "haar";
    case Transform::Cdf53:
      return "cdf53";
    case Transform::Laplacian:
    case Transform::Independent:
      return to_string(spec.opt.downsampler);  // "box" or "gauss"
  }
  return "unknown";
}

// Every level vs the plain 2×2-mean (haar) pyramid; saves one level-3 sample
// per level definition.
void level_quality_table(Run& run, std::ostringstream& md) {
  const auto haar = build_pyramid(run.img, run.args.levels, Transform::Haar,
                                  Downsampler::Box);
  Table t{{"codec"}, {}};
  for (int level = 1; level < run.args.levels; ++level) {
    t.head.push_back(level_name(level));
  }
  const fs::path samples = fs::path(run.args.out).parent_path() / "samples";
  fs::create_directories(samples);
  std::map<std::string, std::string> definition_of;  // codec name -> definition
  for (const auto& spec : all_specs(run.args.levels, run.color)) {
    definition_of[spec.name] = level_definition(spec);
  }
  std::map<std::string, std::vector<std::string>> sampled;  // definition -> codecs

  for (const auto& n : run.args.codecs) {
    if (!run.dec.count(n)) {
      continue;
    }
    std::vector<std::string> row{n};
    for (int level = 1; level < run.args.levels; ++level) {
      const LevelInfo li = run.dec[n]->level_info(level);
      const Image decoded =
          run.dec[n]->decode(level, {0, 0, li.width, li.height}).image;
      row.push_back(psnr_s(psnr(decoded, haar[size_t(level)])));

      const std::string& def = definition_of.at(n);
      if (level == 3) {
        if (!sampled.count(def)) {
          const int s = std::min(256, li.width);
          const Rect center{(li.width - s) / 2, (li.height - s) / 2, s, s};
          save_png((samples / (def + "_" + run.cname + "_L3.png")).string(),
                   decoded.crop(center));
        }
        sampled[def].push_back(n);
      }
    }
    t.rows.push_back(row);
  }

  md << "### Level definitions compared: PSNR of each codec's level vs the "
        "2×2-mean pyramid\n\n"
     << "Level 0 is identical (lossless) for every codec. Codecs with the same "
        "level definition decode identical levels, so the center 256² of level 3 "
        "is saved once per definition in `"
     << samples.string() << "/`:\n\n";
  for (const auto& [def, codecs] : sampled) {
    md << "- `" << def << "_" << run.cname << "_L3.png`:";
    for (const auto& c : codecs) {
      md << " " << c;
    }
    md << "\n";
  }
  md << "\n" << t.str() << "\n";
}

// .prmd (--quant) vs JPEG2000 9/7.
void lossy_table(Run& run, std::ostringstream& md) {
  const Args& a = run.args;
  Table t{{"codec", "setting", "file size", "bits/px", "PSNR L0", "PSNR L2",
           "max |err| L0", "mean 256² L0", "p95 256² L0"},
          {}};

  auto add_row = [&](const std::string& name, const std::string& setting,
                     const std::string& p, uint64_t bytes,
                     const Image* exact_l2) {
    auto d = open_decoder(p);
    const Image l0 = d->decode(0, {0, 0, a.size, a.size}).image;
    std::string psnr_l2 = "n/a";
    if (exact_l2) {
      const LevelInfo li = d->level_info(2);
      psnr_l2 = psnr_s(
          psnr(d->decode(2, {0, 0, li.width, li.height}).image, *exact_l2));
    }
    const auto m = measure(*d, 0, 256, a.iters, a.seed);
    t.rows.push_back({name, setting, bytes_h(double(bytes)),
                      fmt("%.3f", run.bits_per_px(double(bytes))),
                      psnr_s(psnr(l0, run.img)), psnr_l2,
                      std::to_string(max_abs_error(l0, run.img)), ms(m.mean_ms),
                      ms(m.p95)});
  };

  struct LossyMode {
    const char* name;
    Transform t;
    Downsampler d;
    Entropy e;
    int level;
  };
  const LossyMode modes[] = {
      {"haar", Transform::Haar, Downsampler::Box, Entropy::Zstd, 0},
      {"cdf53", Transform::Cdf53, Downsampler::Box, Entropy::Zstd, 0},
      {"laplacian-gauss", Transform::Laplacian, Downsampler::Gauss,
       Entropy::Zstd, 0},
      {"cdf53+range", Transform::Cdf53, Downsampler::Box, Entropy::Range, 0}};

  for (const auto& m : modes) {
    const auto exact = build_pyramid(run.img, a.levels, m.t, m.d);
    for (int q : {2, 4, 8}) {
      if (m.e == Entropy::Range && q != 4) {
        continue;
      }
      EncodeOptions o =
          make_prmd_options(a.levels, run.color, m.t, m.d, m.e, m.level);
      o.quant = q;
      const std::string p = (run.work / (std::string("lossy_") + m.name + "_q" +
                                         std::to_string(q) + ".prmd"))
                                .string();
      std::fprintf(stderr, "[%s] lossy %s q%d...\n", run.cname.c_str(), m.name,
                   q);
      const auto r = encode_file(run.img, p, Codec::Prmd, o);
      add_row(m.name, "quant " + std::to_string(q), p, r.file_bytes, &exact[2]);
    }
  }

  for (float target : {50.f, 45.f, 40.f, 35.f}) {
    EncodeOptions o = make_j2k_options(a.levels, run.color, 256);
    o.j2k_lossless = false;
    o.j2k_psnr = target;
    const std::string p =
        (run.work / ("j2k_lossy_" + std::to_string(int(target)) + ".j2k"))
            .string();
    std::fprintf(stderr, "[%s] lossy j2k %.0f dB...\n", run.cname.c_str(),
                 double(target));
    const auto r = encode_file(run.img, p, Codec::J2K, o);
    add_row("j2k-t256 9/7", fmt("target %.0f dB", target), p, r.file_bytes,
            nullptr);
  }

  md << "### Lossy: .prmd quantization vs JPEG2000 9/7\n\n"
     << "PSNR L0 is against the source (the same for every codec). PSNR L2 is "
        "against that codec's own lossless level 2, i.e. the error added at a "
        "coarse level (n/a for J2K: lossy J2K uses the 9/7 filter, so its "
        "levels are different images from its lossless 5/3 levels). The "
        "coarsest level of .prmd stays lossless.\n\n"
     << t.str() << "\n";
}

void run_color(const Args& a,
               ColorFormat color,
               const Image& source,
               std::ostringstream& md) {
  Run run(a, color, source);
  fs::create_directories(run.work);

  md << "\n## " << run.cname << " — " << a.size << "×" << a.size << ", "
     << a.levels << " levels\n\n";

  encode_all(run);
  file_size_table(run, md);
  level_bytes_table(run, md);

  for (const auto& [n, p] : run.path) {
    run.dec[n] = open_decoder(p);
  }
  latency_tables(run, md);
  if (a.entropy_table) {
    entropy_table(run, md);
  }
  trajectory_table(run, md);
  full_decode_table(run, md);
  level_quality_table(run, md);
  if (a.lossy) {
    lossy_table(run, md);
  }

  if (a.workdir.empty()) {
    fs::remove_all(run.work);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse(argc, argv);
  const Image src = load_image(a.image);

  std::ostringstream md;
  md << "# Prmd benchmark results\n\n";
  md << "- input: `" << a.image << "` (" << src.width << "×" << src.height
     << " " << info(src.format).name << "), center " << a.size << "×" << a.size
     << "\n";
  md << "- CPU: " << cpu_model() << ", " << sysconf(_SC_NPROCESSORS_ONLN)
     << " cores (single-threaded decode)\n";
  md << "- build: " << __VERSION__ << ", Release -O2 -mcpu=native\n";
  md << "- seed " << a.seed << ", " << a.iters
     << " crops per latency row, file in page cache\n";
  md << "- tiles 256×256 for every codec (prmd code-blocks 128×128), J2K "
        "code-blocks 64×64\n";

  if (a.color == "gray" || a.color == "both") {
    run_color(a, ColorFormat::Gray8, src, md);
  }
  if (a.color == "rgb" || a.color == "both") {
    run_color(a, ColorFormat::RGB8, src, md);
  }

  std::fputs(md.str().c_str(), stdout);
  if (!a.out.empty()) {
    if (fs::path(a.out).has_parent_path()) {
      fs::create_directories(fs::path(a.out).parent_path());
    }
    std::ofstream(a.out) << md.str();
    std::fprintf(stderr, "wrote %s\n", a.out.c_str());
  }
  return 0;
}
