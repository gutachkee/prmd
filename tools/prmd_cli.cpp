// prmd — command-line tool: encode an image, decode a crop to PNG, inspect a file.
//
// Exit codes: 0 ok, 1 usage error, 2 I/O error, 3 format error, 4 out of range.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
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

// --key value / --flag parser. Positional args are returned in order.
struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> kv;
  std::map<std::string, bool> flags;

  Args(int argc, char** argv, int start, const std::vector<std::string>& flag_names) {
    for (int i = start; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) == 0 || a == "-o") {
        const std::string key = a == "-o" ? "o" : a.substr(2);
        bool is_flag = false;
        for (const auto& f : flag_names) is_flag |= (f == key);
        if (is_flag) {
          flags[key] = true;
        } else {
          if (i + 1 >= argc) throw UsageError("missing value for " + a);
          kv[key] = argv[++i];
        }
      } else {
        pos.push_back(a);
      }
    }
  }
  bool has(const std::string& k) const { return kv.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def) const { return has(k) ? kv.at(k) : def; }
  int num(const std::string& k, int def) const {
    if (!has(k)) return def;
    char* end = nullptr;
    const long v = std::strtol(kv.at(k).c_str(), &end, 10);
    if (*end != 0) throw UsageError("--" + k + " expects an integer, got '" + kv.at(k) + "'");
    return int(v);
  }
  int required(const std::string& k) const {
    if (!has(k)) throw UsageError("missing required --" + k);
    return num(k, 0);
  }
  bool flag(const std::string& k) const { return flags.count(k) != 0; }
  void check_known(const std::vector<std::string>& known) const {
    for (const auto& [k, v] : kv) {
      bool ok = false;
      for (const auto& n : known) ok |= (n == k);
      if (!ok) throw UsageError("unknown option --" + k);
    }
  }
};

void print_report(const EncodeReport& r, const Image& src, ColorFormat stored, int levels) {
  const double raw0 = double(src.width) * src.height * channels(stored);
  double rawpyr = 0;
  for (int L = 0, w = src.width, h = src.height; L < levels; ++L, w = (w + 1) / 2, h = (h + 1) / 2)
    rawpyr += double(w) * h * channels(stored);
  std::printf("file bytes      %llu\n", (unsigned long long)r.file_bytes);
  std::printf("raw level 0     %.0f  (ratio %.3f, %.3f bits/px)\n", raw0, raw0 / double(r.file_bytes),
              8.0 * double(r.file_bytes) / (double(src.width) * src.height));
  std::printf("raw pyramid     %.0f  (ratio %.3f)\n", rawpyr, rawpyr / double(r.file_bytes));
  for (int L = 0; L < levels; ++L)
    if (r.level_bytes[size_t(L)]) std::printf("  level %d bytes %llu\n", L, (unsigned long long)r.level_bytes[size_t(L)]);
  std::printf("encode time     %.1f ms\n", r.encode_ms);
}

int cmd_encode(int argc, char** argv) {
  Args a(argc, argv, 2, {});
  a.check_known({"codec", "color", "mode", "down", "levels", "tile", "cblk", "quant", "entropy", "entropy-level",
                 "j2k-tile", "j2k-lossy-psnr"});
  if (a.pos.size() != 2) throw UsageError("encode needs <in> <out>");
  EncodeOptions o;
  o.levels = a.num("levels", 5);
  o.tile = a.num("tile", 256);
  o.cblk = a.num("cblk", 128);
  o.quant = a.num("quant", 0);
  o.entropy_level = a.num("entropy-level", 0);
  o.j2k_tile = a.num("j2k-tile", 256);
  const std::string color = a.str("color", "gray"), mode = a.str("mode", "haar"), down = a.str("down", "box");
  const std::string entropy = a.str("entropy", "zstd"), codec = a.str("codec", "prmd");
  if (color == "gray") o.color = ColorFormat::Gray8;
  else if (color == "rgb") o.color = ColorFormat::RGB8;
  else throw UsageError("--color must be gray|rgb");
  if (mode == "haar") o.transform = Transform::Haar;
  else if (mode == "cdf53") o.transform = Transform::Cdf53;
  else if (mode == "laplacian") o.transform = Transform::Laplacian;
  else if (mode == "naive") o.transform = Transform::Independent;
  else throw UsageError("--mode must be haar|cdf53|laplacian|naive");
  if (down == "box") o.downsampler = Downsampler::Box;
  else if (down == "gauss") o.downsampler = Downsampler::Gauss;
  else throw UsageError("--down must be box|gauss");
  if (entropy == "range") o.entropy = Entropy::Range;
  else if (entropy == "zstd") o.entropy = Entropy::Zstd;
  else if (entropy == "raw") o.entropy = Entropy::Raw;
  else throw UsageError("--entropy must be range|zstd|raw");
  if (a.has("j2k-lossy-psnr")) {
    o.j2k_lossless = false;
    o.j2k_psnr = std::stof(a.kv.at("j2k-lossy-psnr"));
  }
  Codec c;
  if (codec == "prmd") c = Codec::Prmd;
  else if (codec == "j2k") c = Codec::J2K;
  else throw UsageError("--codec must be prmd|j2k");

  const Image src = load_image(a.pos[0]);
  std::printf("input           %s  %dx%d %s\n", a.pos[0].c_str(), src.width, src.height, info(src.format).name);
  const EncodeReport r = encode_file(src, a.pos[1], c, o);
  std::printf("output          %s\n", a.pos[1].c_str());
  print_report(r, src, o.color, o.levels);
  return 0;
}

void print_stats(const DecodeStats& s, int levels) {
  std::printf("tiles touched   %d\n", s.tiles_touched);
  std::printf("blocks decoded  %d  (per level:", s.blocks_decoded);
  for (int L = 0; L < levels; ++L) std::printf(" L%d=%d", L, s.blocks_per_level[size_t(L)]);
  std::printf(")\n");
  std::printf("bytes read      %llu in %d read call(s)\n", (unsigned long long)s.bytes_read, s.read_calls);
  if (s.cache_hits) std::printf("cache hits      %d\n", s.cache_hits);
}

int cmd_decode(int argc, char** argv) {
  Args a(argc, argv, 2, {"clamp", "stats"});
  a.check_known({"level", "x", "y", "w", "h", "o"});
  if (a.pos.size() != 1) throw UsageError("decode needs <file>");
  if (!a.has("o")) throw UsageError("missing -o <crop.png>");
  const int L = a.required("level");
  const Rect r{a.required("x"), a.required("y"), a.required("w"), a.required("h")};
  auto dec = open_decoder(a.pos[0]);
  const DecodeResult res = a.flag("clamp") ? dec->decode_clamped(L, r) : dec->decode(L, r);
  save_png(a.kv.at("o"), res.image);
  std::printf("wrote %s  (%dx%d at level %d, rect {%d,%d,%d,%d})\n", a.kv.at("o").c_str(), res.image.width,
              res.image.height, L, res.rect.x, res.rect.y, res.rect.w, res.rect.h);
  if (a.flag("stats")) {
    std::printf("codec           %s\n", dec->name().c_str());
    print_stats(res.stats, dec->num_levels());
  }
  return 0;
}

int cmd_info(int argc, char** argv) {
  Args a(argc, argv, 2, {});
  a.check_known({});
  if (a.pos.size() != 1) throw UsageError("info needs <file>");
  auto dec = open_decoder(a.pos[0]);
  std::printf("codec           %s\n", dec->name().c_str());
  std::printf("color           %s\n", info(dec->color_format()).name);
  std::printf("file bytes      %llu (header+index %llu)\n", (unsigned long long)dec->file_bytes(),
              (unsigned long long)dec->open_bytes_read());
  std::printf("levels          %d\n", dec->num_levels());

  // .prmd: per-level block counts and payload bytes from the index.
  std::ifstream f(a.pos[0], std::ios::binary);
  std::vector<uint8_t> head(internal::kHeaderSize);
  f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
  const bool is_prmd = f && std::memcmp(head.data(), internal::kMagic, 4) == 0;
  internal::PrmdHeader h;
  std::vector<uint32_t> offsets;
  std::vector<internal::LevelGeom> geom;
  if (is_prmd) {
    h = internal::parse_header(head.data(), head.size());
    std::vector<uint8_t> idx(internal::index_bytes(h));
    f.read(reinterpret_cast<char*>(idx.data()), std::streamsize(idx.size()));
    std::vector<internal::LevelEntry> table;
    internal::parse_index(h, idx.data(), idx.size(), table, offsets);
    geom = internal::make_geometry(int(h.width), int(h.height), h.levels, h.tile, h.cblk, h.transform);
    std::printf("tile / cblk     %d / %d\n", h.tile, h.cblk);
    std::printf("entropy         %s (level %d)\n", to_string(h.entropy), h.entropy_level);
    std::printf("quant           %d%s\n", h.quant, h.quant ? " (lossy)" : " (lossless)");
  }
  for (int L = 0; L < dec->num_levels(); ++L) {
    const LevelInfo li = dec->level_info(L);
    std::printf("  level %d  %6d x %-6d", L, li.width, li.height);
    if (is_prmd) {
      const auto& g = geom[size_t(L)];
      const uint64_t bytes = uint64_t(offsets[g.first_block + g.num_ids]) - offsets[g.first_block];
      std::printf("  %s  %4d blocks  %10llu bytes  %.3f bits/px", g.base ? "base  " : "detail", g.blocks_x * g.blocks_y,
                  (unsigned long long)bytes, 8.0 * double(bytes) / (double(li.width) * li.height));
    }
    std::printf("\n");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2) throw UsageError("missing command");
    const std::string cmd = argv[1];
    if (cmd == "encode") return cmd_encode(argc, argv);
    if (cmd == "decode") return cmd_decode(argc, argv);
    if (cmd == "info") return cmd_info(argc, argv);
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
