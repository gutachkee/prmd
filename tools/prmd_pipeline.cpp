// prmdpipeline — times one "encode once, read a few crops" session, step by
// step. This is the program behind docs/pipeline_durations.md.
//
// Every run does, in one process and in this order:
//   1. load_image(input)
//   2. encode_file(..., .prmd) with the chosen options
//   3. open_decoder(file)
//   4. levels + 4 crops of 256×256 with decode_clamped(), around the point at
//      60% / 40% of the image: the whole coarsest level (overview), one crop
//      per level zooming in to L0, then at L0 pans of +128, +256 and
//      +256/+128 px and a jump of −2000/+1200 px.
//
// Two kinds of timing:
//   * Totals: this program reads std::chrono::steady_clock around each of the
//     four steps and each crop. Always on.
//   * Breakdown: the PRMD_TIMED scopes inside the library
//     (include/prmd/profile.h). Only when built with -DPRMD_PROFILE=ON;
//     otherwise the breakdown section is omitted.
// Each number is the median over --runs runs. The timers add a little time,
// so take totals from a build without them and shares from a build with them.
//
// usage: prmdpipeline <image> [--levels 6] [--mode haar|cdf53|laplacian|naive]
//          [--down box|gauss] [--color gray|rgb] [--quant Q]
//          [--entropy zstd|range|raw] [--entropy-level N] [--tile 256]
//          [--cblk 128] [--cache-mib 0] [--runs 5] [--file out.prmd]
//          [--label text]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/image_io.h"
#include "prmd/profile.h"

using namespace prmd;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {
  if (v.empty()) {
    return 0;
  }
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

struct Options {
  std::string input;
  std::string file = "pipeline.prmd";
  std::string label;
  EncodeOptions enc;
  size_t cache_bytes = 0;
  int runs = 5;
};

[[noreturn]] void usage(const std::string& why) {
  std::fprintf(stderr,
               "prmdpipeline: %s\n"
               "usage: prmdpipeline <image> [--levels 6] [--mode "
               "haar|cdf53|laplacian|naive] [--down box|gauss]\n"
               "         [--color gray|rgb] [--quant Q] [--entropy "
               "zstd|range|raw] [--entropy-level N]\n"
               "         [--tile 256] [--cblk 128] [--cache-mib 0] [--runs 5] "
               "[--file out.prmd] [--label text]\n",
               why.c_str());
  std::exit(1);
}

Options parse(int argc, char** argv) {
  Options o;
  o.enc.levels = 6;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--", 0) != 0) {
      if (!o.input.empty()) {
        usage("more than one input");
      }
      o.input = a;
      continue;
    }
    if (i + 1 >= argc) {
      usage(a + " needs a value");
    }
    const std::string v = argv[++i];
    if (a == "--levels") {
      o.enc.levels = std::stoi(v);
    } else if (a == "--mode") {
      if (v == "haar") {
        o.enc.transform = Transform::Haar;
      } else if (v == "cdf53") {
        o.enc.transform = Transform::Cdf53;
      } else if (v == "laplacian") {
        o.enc.transform = Transform::Laplacian;
      } else if (v == "naive") {
        o.enc.transform = Transform::Independent;
      } else {
        usage("bad --mode " + v);
      }
    } else if (a == "--down") {
      o.enc.downsampler = v == "gauss" ? Downsampler::Gauss : Downsampler::Box;
    } else if (a == "--color") {
      o.enc.color = v == "rgb" ? ColorFormat::RGB8 : ColorFormat::Gray8;
    } else if (a == "--quant") {
      o.enc.quant = std::stoi(v);
    } else if (a == "--entropy") {
      if (v == "zstd") {
        o.enc.entropy = Entropy::Zstd;
      } else if (v == "range") {
        o.enc.entropy = Entropy::Range;
      } else if (v == "raw") {
        o.enc.entropy = Entropy::Raw;
      } else {
        usage("bad --entropy " + v);
      }
    } else if (a == "--entropy-level") {
      o.enc.entropy_level = std::stoi(v);
    } else if (a == "--tile") {
      o.enc.tile = std::stoi(v);
    } else if (a == "--cblk") {
      o.enc.cblk = std::stoi(v);
    } else if (a == "--cache-mib") {
      o.cache_bytes = size_t(std::stoul(v)) << 20;
    } else if (a == "--runs") {
      o.runs = std::max(1, std::stoi(v));
    } else if (a == "--file") {
      o.file = v;
    } else if (a == "--label") {
      o.label = v;
    } else {
      usage("unknown option " + a);
    }
  }
  if (o.input.empty()) {
    usage("no input image");
  }
  return o;
}

struct Crop {
  int level;
  const char* step;
  Rect rect;
};

// The crop sequence described at the top of this file.
std::vector<Crop> crop_plan(const CropDecoder& dec) {
  const int top = dec.num_levels() - 1;
  const int s = 256;
  std::vector<Crop> plan;
  Rect l0{};
  for (int level = top; level >= 0; --level) {
    const LevelInfo li = dec.level_info(level);
    const int cx = int(0.6 * li.width);
    const int cy = int(0.4 * li.height);
    const Rect r{std::clamp(cx - s / 2, 0, std::max(0, li.width - s)),
                 std::clamp(cy - s / 2, 0, std::max(0, li.height - s)), s, s};
    plan.push_back({level, level == top ? "overview" : "zoom", r});
    l0 = r;
  }
  plan.push_back({0, "pan +128", {l0.x + 128, l0.y, s, s}});
  plan.push_back({0, "pan +256", {l0.x + 256, l0.y, s, s}});
  plan.push_back({0, "pan +256, +128", {l0.x + 256, l0.y + 128, s, s}});
  plan.push_back({0, "jump", {l0.x - 2000, l0.y + 1200, s, s}});
  return plan;
}

struct CropResult {
  Rect rect;
  double ms = 0;
  DecodeStats stats;
};

struct Run {
  double load_ms = 0, encode_ms = 0, open_ms = 0;
  uint64_t file_bytes = 0;
  std::vector<CropResult> crops;
  std::vector<profile::Entry> profile;
};

Run run_once(const Options& o, std::vector<Crop>& plan) {
  Run run;
  profile::reset();

  auto t0 = Clock::now();
  const Image img = load_image(o.input);
  run.load_ms = ms_since(t0);

  t0 = Clock::now();
  const EncodeReport rep = encode_file(img, o.file, Codec::Prmd, o.enc);
  run.encode_ms = ms_since(t0);
  run.file_bytes = rep.file_bytes;

  DecoderOptions dopt;
  dopt.cache_bytes = o.cache_bytes;
  t0 = Clock::now();
  auto dec = open_decoder(o.file, dopt);
  run.open_ms = ms_since(t0);

  if (plan.empty()) {
    plan = crop_plan(*dec);
  }
  for (const Crop& c : plan) {
    t0 = Clock::now();
    const DecodeResult r = dec->decode_clamped(c.level, c.rect);
    CropResult cr;
    cr.ms = ms_since(t0);
    cr.rect = r.rect;
    cr.stats = r.stats;
    run.crops.push_back(cr);
  }
  run.profile = profile::snapshot();
  return run;
}

std::string fmt_ms(double ms) {
  char b[32];
  if (ms >= 100) {
    std::snprintf(b, sizeof(b), "%.0f", ms);
  } else if (ms >= 10) {
    std::snprintf(b, sizeof(b), "%.1f", ms);
  } else {
    std::snprintf(b, sizeof(b), "%.2f", ms);
  }
  return b;
}

std::string pct(double part, double whole) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.0f%%", whole > 0 ? 100 * part / whole : 0.0);
  return b;
}

// "└ " marks a nested step; deeper levels are indented with em spaces
// (U+2003), which Markdown keeps in table cells, unlike plain spaces.
std::string indent(int depth) {
  if (depth == 0) {
    return "";
  }
  std::string s;
  for (int i = 1; i < depth; ++i) {
    s += "\u2003";
  }
  return s + "└ ";
}

// Median profile tree over runs, with "(other)" rows for a parent's time not
// covered by its children (when at least 1% of the parent).
struct Row {
  std::string path, name;
  int depth;
  uint64_t calls;
  double ms;
};

std::vector<Row> median_tree(const std::vector<Run>& runs) {
  std::map<std::string, std::vector<double>> ms;
  for (const Run& r : runs) {
    for (const auto& e : r.profile) {
      ms[e.path].push_back(e.ms);
    }
  }
  std::vector<Row> rows;
  for (const auto& e : runs.front().profile) {
    rows.push_back({e.path, e.name, e.depth, e.calls, median(ms[e.path])});
  }

  // Copies rows[i] and its subtree to `out`, then the parent's uncovered
  // time; returns the index just past the subtree.
  std::vector<Row> out;
  std::function<size_t(size_t)> emit = [&](size_t i) -> size_t {
    const Row& r = rows[i];
    out.push_back(r);
    double children = 0;
    size_t j = i + 1;
    while (j < rows.size() && rows[j].depth > r.depth) {
      children += rows[j].ms;  // rows[j] is a direct child
      j = emit(j);
    }
    if (j > i + 1 && r.ms - children >= 0.01 * r.ms) {
      out.push_back(
          {r.path + "/(other)", "(other)", r.depth + 1, 0, r.ms - children});
    }
    return j;
  };
  for (size_t i = 0; i < rows.size();) {
    i = emit(i);
  }
  return out;
}

double total_of(const std::vector<Row>& rows, const std::string& path) {
  for (const auto& r : rows) {
    if (r.path == path) {
      return r.ms;
    }
  }
  return 0;
}

// Largest leaves (rows with no children) under the subtree `root`, as
// "name share" strings, largest first.
std::string largest_leaves(const std::vector<Row>& rows,
                           const std::string& root,
                           int count) {
  const double whole = total_of(rows, root);
  std::vector<const Row*> leaves;
  for (size_t i = 0; i < rows.size(); ++i) {
    const bool under = rows[i].path.rfind(root + "/", 0) == 0;
    const bool leaf =
        i + 1 == rows.size() || rows[i + 1].depth <= rows[i].depth;
    if (under && leaf) {
      leaves.push_back(&rows[i]);
    }
  }
  std::sort(leaves.begin(), leaves.end(),
            [](const Row* a, const Row* b) { return a->ms > b->ms; });
  std::string s;
  for (int i = 0; i < count && i < int(leaves.size()); ++i) {
    std::string name = leaves[size_t(i)]->path.substr(root.size() + 1);
    if (!s.empty()) {
      s += ", ";
    }
    s += name + " " + pct(leaves[size_t(i)]->ms, whole);
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const Options o = parse(argc, argv);

  std::vector<Crop> plan;
  std::vector<Run> runs;
  try {
    for (int i = 0; i < o.runs; ++i) {
      runs.push_back(run_once(o, plan));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "prmdpipeline: %s\n", e.what());
    return 2;
  }

  auto med = [&](auto get) {
    std::vector<double> v;
    for (const Run& r : runs) {
      v.push_back(get(r));
    }
    return median(v);
  };
  const double load = med([](const Run& r) { return r.load_ms; });
  const double encode = med([](const Run& r) { return r.encode_ms; });
  const double open = med([](const Run& r) { return r.open_ms; });
  std::vector<double> crop_ms(plan.size());
  double crops = 0;
  for (size_t c = 0; c < plan.size(); ++c) {
    crop_ms[c] = med([&](const Run& r) { return r.crops[c].ms; });
    crops += crop_ms[c];
  }
  const double mib = double(runs.front().file_bytes) / (1 << 20);

  std::printf("## %s\n\n", o.label.empty() ? o.input.c_str() : o.label.c_str());
  std::printf("timers: %s · runs: %d (median) · file: %.2f MiB\n\n",
              profile::kEnabled ? "on (PRMD_PROFILE)" : "off", o.runs, mib);

  std::printf("| step | ms |\n|---|---:|\n");
  std::printf("| load_image | %s |\n", fmt_ms(load).c_str());
  std::printf("| encode_file | %s |\n", fmt_ms(encode).c_str());
  std::printf("| open_decoder | %s |\n", fmt_ms(open).c_str());
  std::printf("| %zu crops | %s |\n", plan.size(), fmt_ms(crops).c_str());
  std::printf("| **total** | **%s** |\n\n",
              fmt_ms(load + encode + open + crops).c_str());

  std::printf(
      "| # | level | step | rect (x, y, w × h) | ms | blocks | KiB read | "
      "reads | cache hits |\n|---|---|---|---|---:|---:|---:|---:|---:|\n");
  int blocks = 0, reads = 0, hits = 0;
  double kib = 0;
  for (size_t c = 0; c < plan.size(); ++c) {
    const CropResult& cr = runs.front().crops[c];
    blocks += cr.stats.blocks_decoded;
    reads += cr.stats.read_calls;
    hits += cr.stats.cache_hits;
    kib += double(cr.stats.bytes_read) / 1024;
    std::printf("| %zu | L%d | %s | %d, %d, %d × %d | %s | %d | %.0f | %d | %d |\n",
                c + 1, plan[c].level, plan[c].step, cr.rect.x, cr.rect.y,
                cr.rect.w, cr.rect.h, fmt_ms(crop_ms[c]).c_str(),
                cr.stats.blocks_decoded, double(cr.stats.bytes_read) / 1024,
                cr.stats.read_calls, cr.stats.cache_hits);
  }
  std::printf("| | | **total** | | **%s** | %d | %.0f | %d | %d |\n\n",
              fmt_ms(crops).c_str(), blocks, kib, reads, hits);

  if (!profile::kEnabled) {
    std::printf(
        "(no breakdown: built without PRMD_PROFILE; configure with "
        "-DPRMD_PROFILE=ON)\n\n");
    std::fprintf(stderr, "SUMMARY\t%s\t%.2f\t%.1f\t%.2f\t\t\n",
                 o.label.c_str(), mib, encode, crops);
    return 0;
  }

  const std::vector<Row> rows = median_tree(runs);
  std::printf(
      "| step | calls | ms | share of parent |\n|---|---:|---:|---:|\n");
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    double parent = 0;
    for (size_t k = i; k-- > 0;) {
      if (rows[k].depth == r.depth - 1) {
        parent = rows[k].ms;
        break;
      }
    }
    std::printf("| %s%s | %s | %s | %s |\n", indent(r.depth).c_str(),
                r.name.c_str(),
                r.calls ? std::to_string(r.calls).c_str() : "",
                fmt_ms(r.ms).c_str(),
                r.depth ? pct(r.ms, parent).c_str() : "");
  }
  std::printf("\n");
  std::fprintf(stderr, "SUMMARY\t%s\t%.2f\t%.1f\t%.2f\t%s\t%s\n",
               o.label.c_str(), mib, encode, crops,
               largest_leaves(rows, "encode_prmd", 2).c_str(),
               largest_leaves(rows, "decode", 2).c_str());
  return 0;
}
