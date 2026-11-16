// prmd — command-line tool: encode an image, decode a crop to PNG, inspect a
// file.
//
// Exit codes: 0 ok, 1 usage error, 2 I/O error, 3 format error, 4 out of
// range.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/errors.h"
#include "prmd/image_io.h"
#include "prmd/internal/prmd_format.h"

using namespace prmd;

namespace {

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

const char* kUsage = R"(usage:
  prmd encode <in.jpg|in.png> <out> [--codec prmd|j2k] [--color gray|rgb]
              [--mode haar|cdf53|laplacian|naive] [--down box|gauss]
              [--levels 5] [--tile 256] [--cblk 128] [--quant Q]
              [--entropy zstd|range|raw] [--entropy-level N]
              [--j2k-tile 256] [--j2k-lossy-psnr DB]
  prmd decode <file> --level L --x X --y Y --w W --h H -o crop.png [--clamp] [--stats]
  prmd info <file>
)";

using Names = std::vector<std::string>;

bool contains(const Names& names, const std::string& name) {
  for (const auto& n : names) {
    if (n == name) {
      return true;
    }
  }
  return false;
}

unsigned long long ull(uint64_t v) {
  return static_cast<unsigned long long>(v);
}

// --key value / --flag parser. Positional args are returned in order.
struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> kv;
  std::map<std::string, bool> flags;

  Args(int argc, char** argv, int start, const Names& flag_names) {
    for (int i = start; i < argc; ++i) {
      const std::string a = argv[i];
      if (a.rfind("--", 0) != 0 && a != "-o") {
        pos.push_back(a);
        continue;
      }

      const std::string key = a == "-o" ? "o" : a.substr(2);
      if (contains(flag_names, key)) {
        flags[key] = true;
      } else {
        if (i + 1 >= argc) {
          throw UsageError("missing value for " + a);
        }
        kv[key] = argv[++i];
      }
    }
  }

  bool has(const std::string& k) const { return kv.count(k) != 0; }

  std::string str(const std::string& k, const std::string& def) const {
    return has(k) ? kv.at(k) : def;
  }

  int num(const std::string& k, int def) const {
    if (!has(k)) {
      return def;
    }
    char* end = nullptr;
    const long v = std::strtol(kv.at(k).c_str(), &end, 10);
    if (*end != 0) {
      throw UsageError("--" + k + " expects an integer, got '" + kv.at(k) +
                       "'");
    }
    return int(v);
  }

  int required(const std::string& k) const {
    if (!has(k)) {
      throw UsageError("missing required --" + k);
    }
    return num(k, 0);
  }

  bool flag(const std::string& k) const { return flags.count(k) != 0; }

  void check_known(const Names& known) const {
    for (const auto& [k, v] : kv) {
      if (!contains(known, k)) {
        throw UsageError("unknown option --" + k);
      }
    }
  }
};

// Maps the value of --`option` (or `def` when absent) to one of `choices`.
// Throws "--option must be a|b|c" otherwise.
template <class T>
T parse_choice(const Args& a,
               const std::string& option,
               const std::string& def,
               const std::vector<std::pair<std::string, T>>& choices) {
  const std::string value = a.str(option, def);
  std::string all;
  for (const auto& [name, v] : choices) {
    if (name == value) {
      return v;
    }
    all += (all.empty() ? "" : "|") + name;
  }
  throw UsageError("--" + option + " must be " + all);
}

// ----------------------------------------------------------------------------
// encode

EncodeOptions parse_encode_options(const Args& a) {
  EncodeOptions o;
  o.levels = a.num("levels", 5);
  o.tile = a.num("tile", 256);
  o.cblk = a.num("cblk", 128);
  o.quant = a.num("quant", 0);
  o.entropy_level = a.num("entropy-level", 0);
  o.j2k_tile = a.num("j2k-tile", 256);

  o.color = parse_choice<ColorFormat>(
      a, "color", "gray",
      {{"gray", ColorFormat::Gray8}, {"rgb", ColorFormat::RGB8}});
  o.transform = parse_choice<Transform>(a, "mode", "haar",
                                        {{"haar", Transform::Haar},
                                         {"cdf53", Transform::Cdf53},
                                         {"laplacian", Transform::Laplacian},
                                         {"naive", Transform::Independent}});
  o.downsampler = parse_choice<Downsampler>(
      a, "down", "box",
      {{"box", Downsampler::Box}, {"gauss", Downsampler::Gauss}});
  o.entropy = parse_choice<Entropy>(a, "entropy", "zstd",
                                    {{"range", Entropy::Range},
                                     {"zstd", Entropy::Zstd},
                                     {"raw", Entropy::Raw}});

  if (a.has("j2k-lossy-psnr")) {
    o.j2k_lossless = false;
    o.j2k_psnr = std::stof(a.kv.at("j2k-lossy-psnr"));
  }
  return o;
}

void print_report(const EncodeReport& r,
                  const Image& src,
                  ColorFormat stored,
                  int levels) {
  const int ch = channels(stored);
  const double pixels0 = double(src.width) * src.height;
  const double raw0 = pixels0 * ch;
  double raw_pyramid = 0;
  for (int level = 0, w = src.width, h = src.height; level < levels;
       ++level, w = (w + 1) / 2, h = (h + 1) / 2) {
    raw_pyramid += double(w) * h * ch;
  }
  const double file_bytes = double(r.file_bytes);

  std::printf("file bytes      %llu\n", ull(r.file_bytes));
  std::printf("raw level 0     %.0f  (ratio %.3f, %.3f bits/px)\n", raw0,
              raw0 / file_bytes, 8.0 * file_bytes / pixels0);
  std::printf("raw pyramid     %.0f  (ratio %.3f)\n", raw_pyramid,
              raw_pyramid / file_bytes);
  for (int level = 0; level < levels; ++level) {
    const uint64_t bytes = r.level_bytes[size_t(level)];
    if (bytes) {
      std::printf("  level %d bytes %llu\n", level, ull(bytes));
    }
  }
  std::printf("encode time     %.1f ms\n", r.encode_ms);
}

int cmd_encode(int argc, char** argv) {
  Args a(argc, argv, 2, {});
  a.check_known({"codec", "color", "mode", "down", "levels", "tile", "cblk",
                 "quant", "entropy", "entropy-level", "j2k-tile",
                 "j2k-lossy-psnr"});
  if (a.pos.size() != 2) {
    throw UsageError("encode needs <in> <out>");
  }

  const EncodeOptions o = parse_encode_options(a);
  const Codec codec = parse_choice<Codec>(
      a, "codec", "prmd", {{"prmd", Codec::Prmd}, {"j2k", Codec::J2K}});

  const Image src = load_image(a.pos[0]);
  std::printf("input           %s  %dx%d %s\n", a.pos[0].c_str(), src.width,
              src.height, info(src.format).name);

  const EncodeReport r = encode_file(src, a.pos[1], codec, o);
  std::printf("output          %s\n", a.pos[1].c_str());
  print_report(r, src, o.color, o.levels);
  return 0;
}

// ----------------------------------------------------------------------------
// decode

void print_stats(const DecodeStats& s, int levels) {
  std::printf("tiles touched   %d\n", s.tiles_touched);
  std::printf("blocks decoded  %d  (per level:", s.blocks_decoded);
  for (int level = 0; level < levels; ++level) {
    std::printf(" L%d=%d", level, s.blocks_per_level[size_t(level)]);
  }
  std::printf(")\n");
  std::printf("bytes read      %llu in %d read call(s)\n", ull(s.bytes_read),
              s.read_calls);
  if (s.cache_hits) {
    std::printf("cache hits      %d\n", s.cache_hits);
  }
}

int cmd_decode(int argc, char** argv) {
  Args a(argc, argv, 2, {"clamp", "stats"});
  a.check_known({"level", "x", "y", "w", "h", "o"});
  if (a.pos.size() != 1) {
    throw UsageError("decode needs <file>");
  }
  if (!a.has("o")) {
    throw UsageError("missing -o <crop.png>");
  }

  const int level = a.required("level");
  const Rect r{a.required("x"), a.required("y"), a.required("w"),
               a.required("h")};
  auto dec = open_decoder(a.pos[0]);
  const DecodeResult res =
      a.flag("clamp") ? dec->decode_clamped(level, r) : dec->decode(level, r);

  const std::string& out_path = a.kv.at("o");
  save_png(out_path, res.image);
  std::printf("wrote %s  (%dx%d at level %d, rect {%d,%d,%d,%d})\n",
              out_path.c_str(), res.image.width, res.image.height, level,
              res.rect.x, res.rect.y, res.rect.w, res.rect.h);

  if (a.flag("stats")) {
    std::printf("codec           %s\n", dec->name().c_str());
    print_stats(res.stats, dec->num_levels());
  }
  return 0;
}

// ----------------------------------------------------------------------------
// info

// Per-level block layout and payload sizes, read from a .prmd index.
struct PrmdIndex {
  internal::PrmdHeader header;
  std::vector<uint32_t> offsets;
  std::vector<internal::LevelGeom> geom;
};

// Returns false if `path` is not a .prmd file.
bool read_prmd_index(const std::string& path, PrmdIndex& out) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> head(internal::kHeaderSize);
  f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  if (!f || std::memcmp(head.data(), internal::kMagic, 4) != 0) {
    return false;
  }

  const internal::PrmdHeader h =
      internal::parse_header(head.data(), head.size());
  std::vector<uint8_t> index(internal::index_bytes(h));
  f.read(reinterpret_cast<char*>(index.data()), std::streamsize(index.size()));

  std::vector<internal::LevelEntry> table;
  internal::parse_index(h, index.data(), index.size(), table, out.offsets);
  out.header = h;
  out.geom = internal::make_geometry(int(h.width), int(h.height), h.levels,
                                     h.tile, h.cblk, h.transform);
  return true;
}

int cmd_info(int argc, char** argv) {
  Args a(argc, argv, 2, {});
  a.check_known({});
  if (a.pos.size() != 1) {
    throw UsageError("info needs <file>");
  }

  auto dec = open_decoder(a.pos[0]);
  std::printf("codec           %s\n", dec->name().c_str());
  std::printf("color           %s\n", info(dec->color_format()).name);
  std::printf("file bytes      %llu (header+index %llu)\n",
              ull(dec->file_bytes()), ull(dec->open_bytes_read()));
  std::printf("levels          %d\n", dec->num_levels());

  PrmdIndex prmd;
  const bool is_prmd = read_prmd_index(a.pos[0], prmd);
  if (is_prmd) {
    const internal::PrmdHeader& h = prmd.header;
    std::printf("tile / cblk     %d / %d\n", h.tile, h.cblk);
    // entropy_level is informational (decoders ignore it); 0 = coder default.
    if (h.entropy != Entropy::Zstd) {
      std::printf("entropy         %s\n", to_string(h.entropy));
    } else if (h.entropy_level == 0) {
      std::printf("entropy         zstd (default level)\n");
    } else {
      std::printf("entropy         zstd (level %d)\n", h.entropy_level);
    }
    std::printf("quant           %d%s\n", h.quant,
                h.quant ? " (lossy)" : " (lossless)");
  }

  for (int level = 0; level < dec->num_levels(); ++level) {
    const LevelInfo li = dec->level_info(level);
    std::printf("  level %d  %6d x %-6d", level, li.width, li.height);
    if (is_prmd) {
      const internal::LevelGeom& g = prmd.geom[size_t(level)];
      const uint64_t bytes = uint64_t(prmd.offsets[g.first_block + g.num_ids]) -
                             prmd.offsets[g.first_block];
      std::printf("  %s  %4d blocks  %10llu bytes  %.3f bits/px",
                  g.base ? "base  " : "detail", g.blocks_x * g.blocks_y,
                  ull(bytes),
                  8.0 * double(bytes) / (double(li.width) * li.height));
    }
    std::printf("\n");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      throw UsageError("missing command");
    }
    const std::string cmd = argv[1];
    if (cmd == "encode") {
      return cmd_encode(argc, argv);
    }
    if (cmd == "decode") {
      return cmd_decode(argc, argv);
    }
    if (cmd == "info") {
      return cmd_info(argc, argv);
    }
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
      std::fputs(kUsage, stdout);
      return 0;
    }
    throw UsageError("unknown command '" + cmd + "'");
  } catch (const UsageError& e) {
    std::fprintf(stderr, "prmd: %s\n%s", e.what(), kUsage);
    return 1;
  } catch (const std::invalid_argument& e) {
    std::fprintf(stderr, "prmd: invalid argument: %s\n", e.what());
    return 1;
  } catch (const IoError& e) {
    std::fprintf(stderr, "prmd: I/O error: %s\n", e.what());
    return 2;
  } catch (const FormatError& e) {
    std::fprintf(stderr, "prmd: format error: %s\n", e.what());
    return 3;
  } catch (const std::out_of_range& e) {
    std::fprintf(stderr, "prmd: out of range: %s\n", e.what());
    return 4;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "prmd: error: %s\n", e.what());
    return 2;
  }
}
