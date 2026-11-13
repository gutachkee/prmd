// prmdbench — size / latency / I/O / quality benchmark for every codec (DESIGN.md §10).
//
// usage: prmdbench <image> [--levels 5] [--iters 300] [--seed 1] [--size 4096]
//                  [--color gray|rgb|both] [--codecs a,b,...] [--out bench/results.md]
//                  [--workdir DIR] [--no-entropy-table] [--lossy]

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/image_io.h"
#include "prmd/pyramid.h"

using namespace prmd;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct CodecSpec {
  std::string name;
  Codec codec;
  EncodeOptions opt;
};

std::vector<CodecSpec> all_specs(int levels, ColorFormat color) {
  auto prmd = [&](const char* n, Transform t, Downsampler d, Entropy e = Entropy::Zstd, int lvl = 0) {
    EncodeOptions o;
    o.levels = levels;
    o.color = color;
    o.transform = t;
    o.downsampler = d;
    o.entropy = e;
    o.entropy_level = lvl;
    return CodecSpec{n, Codec::Prmd, o};
  };
  auto j2k = [&](const char* n, int tile) {
    EncodeOptions o;
    o.levels = levels;
    o.color = color;
    o.j2k_tile = tile;
    return CodecSpec{n, Codec::J2K, o};
  };
  return {
      prmd("haar", Transform::Haar, Downsampler::Box),
      prmd("cdf53", Transform::Cdf53, Downsampler::Box),
      prmd("laplacian-box", Transform::Laplacian, Downsampler::Box),
      prmd("laplacian-gauss", Transform::Laplacian, Downsampler::Gauss),
      prmd("naive", Transform::Independent, Downsampler::Box),
      j2k("j2k-t256", 256),
      j2k("j2k-t1024", 1024),
      // Unsuffixed names use the default coder (zstd, level 19).
      prmd("haar+zstd3", Transform::Haar, Downsampler::Box, Entropy::Zstd, 3),
      prmd("haar+range", Transform::Haar, Downsampler::Box, Entropy::Range),
      prmd("haar+raw", Transform::Haar, Downsampler::Box, Entropy::Raw),
      prmd("cdf53+zstd3", Transform::Cdf53, Downsampler::Box, Entropy::Zstd, 3),
      prmd("cdf53+range", Transform::Cdf53, Downsampler::Box, Entropy::Range),
      prmd("cdf53+raw", Transform::Cdf53, Downsampler::Box, Entropy::Raw),
      prmd("naive+range", Transform::Independent, Downsampler::Box, Entropy::Range),
      prmd("naive+raw", Transform::Independent, Downsampler::Box, Entropy::Raw),
  };
}

const std::vector<std::string> kMainCodecs = {"haar", "cdf53", "laplacian-box", "laplacian-gauss",
                                              "naive", "j2k-t256", "j2k-t1024"};
const std::vector<std::string> kEntropyCodecs = {"haar",  "haar+zstd3",  "haar+range",  "haar+raw",
                                                 "cdf53", "cdf53+zstd3", "cdf53+range", "cdf53+raw",
                                                 "naive", "naive+range", "naive+raw"};

// Center-crop (or edge-pad) to size × size.
Image fit_square(const Image& src, int size) {
  Image out(size, size, src.format);
  const int C = src.channels();
  const int ox = (src.width - size) / 2, oy = (src.height - size) / 2;
  for (int y = 0; y < size; ++y) {
    const int sy = std::clamp(y + oy, 0, src.height - 1);
    for (int x = 0; x < size; ++x) {
      const int sx = std::clamp(x + ox, 0, src.width - 1);
      for (int c = 0; c < C; ++c) out.row(y)[size_t(x) * size_t(C) + size_t(c)] = src.at(sx, sy, c);
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
  if (se == 0) return INFINITY;
  return 10.0 * std::log10(255.0 * 255.0 * double(a.pixels.size()) / se);
}

std::string fmt(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
std::string bytes_h(double b) {
  if (b >= 1 << 20) return fmt("%.2f MiB", b / double(1 << 20));
  if (b >= 1 << 10) return fmt("%.1f KiB", b / 1024.0);
  return fmt("%.0f B", b);
}
std::string psnr_s(double p) { return std::isinf(p) ? "∞ (exact)" : fmt("%.2f dB", p); }

struct Table {
  std::vector<std::string> head;
  std::vector<std::vector<std::string>> rows;
  std::string str() const {
    std::ostringstream o;
    o << "|";
    for (auto& h : head) o << " " << h << " |";
    o << "\n|";
    for (size_t i = 0; i < head.size(); ++i) o << (i == 0 ? "---|" : "---:|");
    o << "\n";
    for (auto& r : rows) {
      o << "|";
      for (auto& c : r) o << " " << c << " |";
      o << "\n";
    }
    return o.str();
  }
};

std::string cpu_model() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  std::string model, impl, part;
  while (std::getline(f, line)) {
    auto val = [&] { return line.substr(line.find(':') + 2); };
    if (line.rfind("model name", 0) == 0 && model.empty()) model = val();
    if (line.rfind("CPU implementer", 0) == 0 && impl.empty()) impl = val();
    if (line.rfind("CPU part", 0) == 0 && part.empty()) part = val();
  }
  if (!model.empty()) return model;
  return "ARM implementer " + impl + " part " + part;
}

struct Args {
  std::string image, out = "bench/results.md", workdir, color = "gray";
  int levels = 5, iters = 300, size = 4096;
  unsigned seed = 1;
  std::vector<std::string> codecs;
  bool entropy_table = true, lossy = false;
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", s.c_str());
        std::exit(1);
      }
      return argv[++i];
    };
    if (s == "--levels") a.levels = std::stoi(next());
    else if (s == "--iters") a.iters = std::stoi(next());
    else if (s == "--seed") a.seed = unsigned(std::stoul(next()));
    else if (s == "--size") a.size = std::stoi(next());
    else if (s == "--color") a.color = next();
    else if (s == "--out") a.out = next();
    else if (s == "--workdir") a.workdir = next();
    else if (s == "--no-entropy-table") a.entropy_table = false;
    else if (s == "--lossy") a.lossy = true;
    else if (s == "--codecs") {
      std::stringstream ss(next());
      std::string c;
      while (std::getline(ss, c, ',')) a.codecs.push_back(c);
    } else if (s.rfind("--", 0) == 0) {
      std::fprintf(stderr, "unknown option %s\n", s.c_str());
      std::exit(1);
    } else a.image = s;
  }
  if (a.image.empty()) {
    std::fprintf(stderr,
                 "usage: prmdbench <image> [--levels 5] [--iters 300] [--seed 1] [--size 4096]\n"
                 "                 [--color gray|rgb|both] [--codecs a,b,...] [--out bench/results.md]\n"
                 "                 [--workdir DIR] [--no-entropy-table] [--lossy]\n");
    std::exit(1);
  }
  if (a.codecs.empty()) a.codecs = kMainCodecs;
  return a;
}

struct LatencyRow {
  double mean_ms = 0, p50 = 0, p95 = 0, tiles = 0, blocks = 0, bytes = 0, reads = 0;
};

LatencyRow measure(CropDecoder& dec, int L, int crop, int iters, unsigned seed) {
  const LevelInfo li = dec.level_info(L);
  std::mt19937 rng(seed * 7919u + unsigned(L) * 131u + unsigned(crop));
  auto rand_rect = [&] {
    return Rect{int(rng() % unsigned(li.width - crop + 1)), int(rng() % unsigned(li.height - crop + 1)), crop, crop};
  };
  for (int i = 0; i < 10; ++i) (void)dec.decode(L, rand_rect());  // warm-up
  std::vector<double> t;
  LatencyRow r;
  for (int i = 0; i < iters; ++i) {
    const Rect rc = rand_rect();
    const auto t0 = Clock::now();
    const DecodeResult res = dec.decode(L, rc);
    t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    r.tiles += res.stats.tiles_touched;
    r.blocks += res.stats.blocks_decoded;
    r.bytes += double(res.stats.bytes_read);
    r.reads += res.stats.read_calls;
  }
  std::sort(t.begin(), t.end());
  for (double v : t) r.mean_ms += v;
  r.mean_ms /= double(iters);
  r.p50 = t[size_t(iters / 2)];
  r.p95 = t[std::min(size_t(iters) - 1, size_t(std::ceil(0.95 * iters)) - 1)];
  r.tiles /= iters;
  r.blocks /= iters;
  r.bytes /= iters;
  r.reads /= iters;
  return r;
}

void run(const Args& a, ColorFormat color, const Image& source, std::ostringstream& md) {
  const std::string cname = info(color).name;
  const Image img = fit_square(convert(source, color), a.size);
  const fs::path work = a.workdir.empty() ? fs::temp_directory_path() / ("prmdbench_" + std::to_string(::getpid()))
                                          : fs::path(a.workdir);
  fs::create_directories(work);
  const int C = img.channels();
  const double raw0 = double(a.size) * a.size * C;
  double rawpyr = 0;
  for (int L = 0, s = a.size; L < a.levels; ++L, s = (s + 1) / 2) rawpyr += double(s) * s * C;

  md << "\n## " << cname << " — " << a.size << "×" << a.size << ", " << a.levels << " levels\n\n";

  // ------------------------------------------------------------ encode everything
  std::vector<std::string> wanted = a.codecs;
  if (a.entropy_table)
    for (auto& n : kEntropyCodecs)
      if (std::find(wanted.begin(), wanted.end(), n) == wanted.end()) wanted.push_back(n);
  std::map<std::string, std::string> path;
  std::map<std::string, EncodeReport> rep;
  Table sizes{{"codec", "file size", "bits/px", "vs raw L0", "vs raw pyramid", "vs naive", "encode"}, {}};
  for (const auto& spec : all_specs(a.levels, color)) {
    if (std::find(wanted.begin(), wanted.end(), spec.name) == wanted.end()) continue;
    const std::string p = (work / (spec.name + "_" + cname + (spec.codec == Codec::J2K ? ".j2k" : ".prmd"))).string();
    std::fprintf(stderr, "[%s] encoding %s...\n", cname.c_str(), spec.name.c_str());
    rep[spec.name] = encode_file(img, p, spec.codec, spec.opt);
    path[spec.name] = p;
  }
  const double naive_bytes = rep.count("naive") ? double(rep["naive"].file_bytes) : 0;

  // PNG per 256-tile of the naive (box) pyramid — size-only reference.
  double png_bytes = 0;
  {
    const auto pyr = build_pyramid(img, a.levels, Transform::Independent, Downsampler::Box);
    const fs::path tp = work / "tile.png";
    for (const auto& lv : pyr)
      for (int y = 0; y < lv.height; y += 256)
        for (int x = 0; x < lv.width; x += 256) {
          save_png(tp.string(), lv.crop(intersect({x, y, 256, 256}, lv.bounds())));
          png_bytes += double(fs::file_size(tp));
        }
    fs::remove(tp);
  }
  auto size_row = [&](const std::string& n, double b, double ms) {
    sizes.rows.push_back({n, bytes_h(b), fmt("%.3f", 8.0 * b / (double(a.size) * a.size)), fmt("%.3f×", raw0 / b),
                          fmt("%.3f×", rawpyr / b), naive_bytes > 0 ? fmt("%+.1f%%", 100.0 * (b / naive_bytes - 1)) : "-",
                          ms > 0 ? fmt("%.0f ms", ms) : "-"});
  };
  for (const auto& n : a.codecs)
    if (rep.count(n)) size_row(n, double(rep[n].file_bytes), rep[n].encode_ms);
  size_row("png-tiles (naive pyramid)", png_bytes, 0);
  sizes.rows.push_back({"raw level 0", bytes_h(raw0), fmt("%.3f", 8.0 * C), "1.000×", "-", "-", "-"});
  sizes.rows.push_back({"raw pyramid", bytes_h(rawpyr), fmt("%.3f", 8.0 * rawpyr / raw0 * C), "-", "1.000×", "-", "-"});
  md << "### File size (lossless; bits/px over level-0 pixels)\n\n" << sizes.str() << "\n";

  // Per-level bytes for .prmd codecs.
  {
    Table t{{"codec"}, {}};
    for (int L = 0; L < a.levels; ++L) t.head.push_back("L" + std::to_string(L));
    for (const auto& n : a.codecs) {
      if (!rep.count(n) || n.rfind("j2k", 0) == 0) continue;
      std::vector<std::string> row{n};
      for (int L = 0; L < a.levels; ++L) row.push_back(bytes_h(double(rep[n].level_bytes[size_t(L)])));
      t.rows.push_back(row);
    }
    md << "### Payload bytes per level (.prmd)\n\n" << t.str() << "\n";
  }

  // ------------------------------------------------------------ latency
  std::map<std::string, std::unique_ptr<CropDecoder>> dec;
  for (const auto& [n, p] : path) dec[n] = open_decoder(p);
  for (int crop : {256, 512}) {
    Table t{{"codec", "level", "mean", "p50", "p95", "tiles touched", "blocks decoded", "bytes read", "read calls"}, {}};
    for (const auto& n : a.codecs) {
      if (!dec.count(n)) continue;
      for (int L = 0; L < a.levels; ++L) {
        const LevelInfo li = dec[n]->level_info(L);
        if (li.width < crop || li.height < crop) continue;
        std::fprintf(stderr, "[%s] %s L%d %d² ...\n", cname.c_str(), n.c_str(), L, crop);
        const auto r = measure(*dec[n], L, crop, a.iters, a.seed);
        t.rows.push_back({n, "L" + std::to_string(L), fmt("%.2f ms", r.mean_ms), fmt("%.2f ms", r.p50),
                          fmt("%.2f ms", r.p95), fmt("%.1f", r.tiles), fmt("%.1f", r.blocks), bytes_h(r.bytes),
                          fmt("%.1f", r.reads)});
      }
    }
    md << "### Random " << crop << "×" << crop << " crops (" << a.iters
       << " per row, cold cache, non-aligned positions)\n\n"
       << t.str() << "\n";
  }

  // Entropy coder comparison.
  if (a.entropy_table) {
    Table t{{"coder", "file size", "vs zstd19 (default)", "vs naive, same coder", "mean 256² L0", "p95 256² L0", "mean 512² L1"}, {}};
    for (const auto& n : kEntropyCodecs) {
      if (!dec.count(n)) continue;
      const std::string base = n.substr(0, n.find('+'));
      const auto r0 = measure(*dec[n], 0, 256, a.iters, a.seed), r1 = measure(*dec[n], 1, 512, a.iters, a.seed);
      t.rows.push_back({n, bytes_h(double(rep[n].file_bytes)),
                        fmt("%+.1f%%", 100.0 * (double(rep[n].file_bytes) / double(rep[base].file_bytes) - 1)),
                        [&]() -> std::string {
                          const auto plus = n.find('+');
                          const std::string nv = "naive" + (plus == std::string::npos ? "" : n.substr(plus));
                          if (!rep.count(nv) || base == "naive") return "-";
                          return fmt("%+.1f%%", 100.0 * (double(rep[n].file_bytes) / double(rep[nv].file_bytes) - 1));
                        }(),
                        fmt("%.2f ms", r0.mean_ms), fmt("%.2f ms", r0.p95), fmt("%.2f ms", r1.mean_ms)});
    }
    md << "### Entropy coders (" << a.iters << " crops per cell)\n\n" << t.str() << "\n";
  }

  // Trajectory with cache (navigation-like random walk).
  {
    Table t{{"codec", "cache", "mean", "p95", "blocks decoded", "cache hits", "bytes read"}, {}};
    for (const auto& n : a.codecs) {
      if (!path.count(n) || n.rfind("j2k", 0) == 0) continue;
      for (size_t cache : {size_t(0), size_t(64) << 20}) {
        DecoderOptions o;
        o.cache_bytes = cache;
        auto d = open_decoder(path[n], o);
        std::mt19937 rng(a.seed);
        int x = a.size / 2, y = a.size / 2;
        std::vector<double> ts;
        double blocks = 0, hits = 0, bytes = 0;
        for (int i = 0; i < 1000; ++i) {
          x = std::clamp(x + int(rng() % 65u) - 32, 0, a.size - 256);
          y = std::clamp(y + int(rng() % 65u) - 32, 0, a.size - 256);
          const auto t0 = Clock::now();
          const auto res = d->decode(0, {x, y, 256, 256});
          ts.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
          blocks += res.stats.blocks_decoded;
          hits += res.stats.cache_hits;
          bytes += double(res.stats.bytes_read);
        }
        double mean = 0;
        for (double v : ts) mean += v;
        std::sort(ts.begin(), ts.end());
        t.rows.push_back({n, cache ? "64 MiB" : "off", fmt("%.2f ms", mean / 1000), fmt("%.2f ms", ts[949]),
                          fmt("%.1f", blocks / 1000), fmt("%.1f", hits / 1000), bytes_h(bytes / 1000)});
      }
    }
    md << "### Trajectory: 1000-step random walk of 256² crops at L0 (±32 px/step)\n\n" << t.str() << "\n";
  }

  // Full-level decode (the "decode everything, then crop" baseline).
  {
    Table t{{"codec", "full L0 decode", "full L1 decode", "full L2 decode"}, {}};
    for (const auto& n : a.codecs) {
      if (!dec.count(n)) continue;
      std::vector<std::string> row{n};
      for (int L = 0; L < 3; ++L) {
        const LevelInfo li = dec[n]->level_info(L);
        const auto t0 = Clock::now();
        (void)dec[n]->decode(L, {0, 0, li.width, li.height});
        row.push_back(fmt("%.1f ms", std::chrono::duration<double, std::milli>(Clock::now() - t0).count()));
      }
      t.rows.push_back(row);
    }
    md << "### Full-level decode (what a crop costs without random access)\n\n" << t.str() << "\n";
  }

  // Level quality: every level vs the plain 2×2-mean (haar) pyramid; save level-3 samples.
  {
    const auto haar = build_pyramid(img, a.levels, Transform::Haar, Downsampler::Box);
    Table t{{"codec"}, {}};
    for (int L = 1; L < a.levels; ++L) t.head.push_back("L" + std::to_string(L));
    const fs::path samples = fs::path(a.out).parent_path() / "samples";
    fs::create_directories(samples);
    for (const auto& n : a.codecs) {
      if (!dec.count(n)) continue;
      std::vector<std::string> row{n};
      for (int L = 1; L < a.levels; ++L) {
        const LevelInfo li = dec[n]->level_info(L);
        const Image lv = dec[n]->decode(L, {0, 0, li.width, li.height}).image;
        row.push_back(psnr_s(psnr(lv, haar[size_t(L)])));
        if (L == 3) {
          const int s = std::min(256, li.width);
          save_png((samples / (n + "_" + cname + "_L3.png")).string(), lv.crop({(li.width - s) / 2, (li.height - s) / 2, s, s}));
        }
      }
      t.rows.push_back(row);
    }
    md << "### Level definitions compared: PSNR of each codec's level vs the 2×2-mean pyramid\n\n"
       << "Level 0 is identical (lossless) for every codec. Center 256² of each level 3 is saved in `"
       << samples.string() << "/`.\n\n"
       << t.str() << "\n";
  }

  // Lossy: .prmd (--quant) vs JPEG2000 9/7.
  if (a.lossy) {
    Table t{{"codec", "setting", "file size", "bits/px", "PSNR L0", "PSNR L2", "max |err| L0", "mean 256² L0",
             "p95 256² L0"},
            {}};
    auto max_err = [](const Image& x, const Image& y) {
      int m = 0;
      for (size_t i = 0; i < x.pixels.size(); ++i) m = std::max(m, std::abs(int(x.pixels[i]) - int(y.pixels[i])));
      return m;
    };
    auto add_row = [&](const std::string& name, const std::string& setting, const std::string& p, uint64_t bytes,
                       const Image* exact_l2) {
      auto d = open_decoder(p);
      const Image l0 = d->decode(0, {0, 0, a.size, a.size}).image;
      std::string p2 = "n/a";
      if (exact_l2) {
        const LevelInfo li = d->level_info(2);
        p2 = psnr_s(psnr(d->decode(2, {0, 0, li.width, li.height}).image, *exact_l2));
      }
      const auto m = measure(*d, 0, 256, a.iters, a.seed);
      t.rows.push_back({name, setting, bytes_h(double(bytes)), fmt("%.3f", 8.0 * double(bytes) / (double(a.size) * a.size)),
                        psnr_s(psnr(l0, img)), p2, std::to_string(max_err(l0, img)), fmt("%.2f ms", m.mean_ms),
                        fmt("%.2f ms", m.p95)});
    };
    struct LossyMode {
      const char* name;
      Transform t;
      Downsampler d;
      Entropy e;
      int level;
    };
    const LossyMode modes[] = {{"haar", Transform::Haar, Downsampler::Box, Entropy::Zstd, 0},
                               {"cdf53", Transform::Cdf53, Downsampler::Box, Entropy::Zstd, 0},
                               {"laplacian-gauss", Transform::Laplacian, Downsampler::Gauss, Entropy::Zstd, 0},
                               {"cdf53+range", Transform::Cdf53, Downsampler::Box, Entropy::Range, 0}};
    for (const auto& m : modes) {
      const auto exact = build_pyramid(img, a.levels, m.t, m.d);
      for (int q : {2, 4, 8}) {
        if (m.e == Entropy::Range && q != 4) continue;
        EncodeOptions o;
        o.levels = a.levels;
        o.color = color;
        o.transform = m.t;
        o.downsampler = m.d;
        o.entropy = m.e;
        o.entropy_level = m.level;
        o.quant = q;
        const std::string p = (work / (std::string("lossy_") + m.name + "_q" + std::to_string(q) + ".prmd")).string();
        std::fprintf(stderr, "[%s] lossy %s q%d...\n", cname.c_str(), m.name, q);
        const auto r = encode_file(img, p, Codec::Prmd, o);
        add_row(m.name, "quant " + std::to_string(q), p, r.file_bytes, &exact[2]);
      }
    }
    for (float target : {50.f, 45.f, 40.f, 35.f}) {
      EncodeOptions o;
      o.levels = a.levels;
      o.color = color;
      o.j2k_tile = 256;
      o.j2k_lossless = false;
      o.j2k_psnr = target;
      const std::string p = (work / ("j2k_lossy_" + std::to_string(int(target)) + ".j2k")).string();
      std::fprintf(stderr, "[%s] lossy j2k %.0f dB...\n", cname.c_str(), double(target));
      const auto r = encode_file(img, p, Codec::J2K, o);
      add_row("j2k-t256 9/7", fmt("target %.0f dB", target), p, r.file_bytes, nullptr);
    }
    md << "### Lossy: .prmd quantization vs JPEG2000 9/7\n\n"
       << "PSNR L0 is against the source (the same for every codec). PSNR L2 is against that codec's own "
          "lossless level 2, i.e. the error added at a coarse level (n/a for J2K: lossy J2K uses the 9/7 filter, so its "
          "levels are different images from its lossless 5/3 levels). The coarsest level of .prmd stays lossless.\n\n"
       << t.str() << "\n";
  }

  if (a.workdir.empty()) fs::remove_all(work);
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = parse(argc, argv);
  const Image src = load_image(a.image);
  std::ostringstream md;
  md << "# Prmd benchmark results\n\n";
  md << "- input: `" << a.image << "` (" << src.width << "×" << src.height << " " << info(src.format).name
     << "), center " << a.size << "×" << a.size << "\n";
  md << "- CPU: " << cpu_model() << ", " << sysconf(_SC_NPROCESSORS_ONLN) << " cores (single-threaded decode)\n";
  md << "- build: " << __VERSION__ << ", Release -O2 -mcpu=native\n";
  md << "- seed " << a.seed << ", " << a.iters << " crops per latency row, file in page cache\n";
  md << "- tiles 256×256 for every codec (prmd code-blocks 128×128), J2K code-blocks 64×64\n";
  if (a.color == "gray" || a.color == "both") run(a, ColorFormat::Gray8, src, md);
  if (a.color == "rgb" || a.color == "both") run(a, ColorFormat::RGB8, src, md);
  std::fputs(md.str().c_str(), stdout);
  if (!a.out.empty()) {
    if (fs::path(a.out).has_parent_path()) fs::create_directories(fs::path(a.out).parent_path());
    std::ofstream(a.out) << md.str();
    std::fprintf(stderr, "wrote %s\n", a.out.c_str());
  }
  return 0;
}
