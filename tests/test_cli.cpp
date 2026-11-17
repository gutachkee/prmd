// The `prmd` command-line tool, run as a child process: exit codes (0 ok,
// 1 usage, 2 I/O, 3 format, 4 out of range), strict number parsing, --clamp
// and the PNG it writes.

#include <gtest/gtest.h>

#include <sys/wait.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "prmd/image_io.h"
#include "reference.h"
#include "test_util.h"

using namespace prmd;

namespace {

// Runs `prmd args...` with stdout and stderr sent to a log file. Returns the
// exit code, or -1 if the process did not exit normally.
int run_cli(const std::vector<std::string>& args) {
  // In sanitizer builds a report must not pass for the usage exit code 1.
  std::string cmd =
      "ASAN_OPTIONS=exitcode=86 UBSAN_OPTIONS=exitcode=86 '" PRMD_CLI_PATH "'";
  for (const std::string& a : args) {
    cmd += " '" + a + "'";  // test arguments contain no single quotes
  }
  cmd += " > '" + testutil::temp_path("cli.log").string() + "' 2>&1";
  const int status = std::system(cmd.c_str());
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// A 300×200 gray PNG and its lossless 3-level .prmd encode, made once.
struct CliFiles {
  Image src;
  std::string png;
  std::string prmd;
};

const CliFiles& files() {
  static const CliFiles f = [] {
    CliFiles c;
    c.src = ref::synthetic_image(300, 200, ColorFormat::Gray8, 7);
    c.png = testutil::temp_path("cli_src.png").string();
    c.prmd = testutil::temp_path("cli.prmd").string();
    save_png(c.png, c.src);
    EXPECT_EQ(run_cli({"encode", c.png, c.prmd, "--levels", "3"}), 0);
    return c;
  }();
  return f;
}

// `decode` arguments for the crop {x, y, w, h} of `level`, written to `out`.
std::vector<std::string> decode_args(const std::string& file,
                                     const std::string& level,
                                     const std::string& x,
                                     const std::string& y,
                                     const std::string& w,
                                     const std::string& h,
                                     const std::string& out) {
  return {"decode", file, "--level", level, "--x", x, "--y", y,
          "--w",    w,    "--h",     h,     "-o", out};
}

}  // namespace

TEST(Cli, ExitCodeZeroForEncodeDecodeInfoAndHelp) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_crop.png").string();
  EXPECT_EQ(run_cli(decode_args(f.prmd, "0", "10", "20", "64", "32", out)), 0);
  EXPECT_EQ(run_cli({"info", f.prmd}), 0);
  EXPECT_EQ(run_cli({"--help"}), 0);
  EXPECT_EQ(run_cli({"encode", f.png, testutil::temp_path("c.j2k").string(),
                     "--codec", "j2k", "--levels", "3"}),
            0);
}

TEST(Cli, DecodeWritesTheExactCropAsPng) {
  const CliFiles& f = files();
  const auto ref = ref::pyramid(f.src, 3, Transform::Haar, Downsampler::Box);
  const std::string out = testutil::temp_path("cli_exact.png").string();

  ASSERT_EQ(run_cli(decode_args(f.prmd, "0", "100", "50", "130", "70", out)),
            0);
  const Image l0 = load_image(out);
  EXPECT_EQ(l0.format, ColorFormat::Gray8);
  EXPECT_EQ(l0.pixels, f.src.crop({100, 50, 130, 70}).pixels);

  ASSERT_EQ(run_cli(decode_args(f.prmd, "2", "5", "7", "60", "40", out)), 0);
  EXPECT_EQ(load_image(out).pixels, ref[2].crop({5, 7, 60, 40}).pixels);
}

TEST(Cli, ClampDecodesTheIntersection) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_clamp.png").string();
  // Level 1 is 150×100: the request sticks out on the right and bottom.
  auto args = decode_args(f.prmd, "1", "120", "80", "64", "64", out);
  EXPECT_EQ(run_cli(args), 4);  // strict by default

  args.push_back("--clamp");
  ASSERT_EQ(run_cli(args), 0);
  const Image img = load_image(out);
  EXPECT_EQ(img.width, 30);
  EXPECT_EQ(img.height, 20);
  const auto ref = ref::pyramid(f.src, 3, Transform::Haar, Downsampler::Box);
  EXPECT_EQ(img.pixels, ref[1].crop({120, 80, 30, 20}).pixels);
}

TEST(Cli, UsageErrorsExitWithOne) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_usage.png").string();
  EXPECT_EQ(run_cli({}), 1);                     // no command
  EXPECT_EQ(run_cli({"frobnicate"}), 1);         // unknown command
  EXPECT_EQ(run_cli({"encode", f.png}), 1);      // missing <out>
  EXPECT_EQ(run_cli({"info", f.prmd, "--bogus", "1"}), 1);  // unknown option
  EXPECT_EQ(run_cli({"decode", f.prmd, "--level", "0", "--x", "0", "--y", "0",
                     "--w", "8", "--h", "8"}),
            1);  // missing -o
  EXPECT_EQ(run_cli({"decode", f.prmd, "--level"}), 1);  // missing value
  EXPECT_EQ(run_cli({"encode", f.png, out, "--mode", "wavelet"}), 1);
  EXPECT_EQ(run_cli({"encode", f.png, out, "--levels", "0"}), 1);  // invalid
}

TEST(Cli, NoMedAndNoYcocgFlags) {
  const CliFiles& f = files();
  const std::string file = testutil::temp_path("cli_flags.prmd").string();
  const std::string out = testutil::temp_path("cli_flags.png").string();
  ASSERT_EQ(run_cli({"encode", f.png, file, "--levels", "3", "--mode", "naive",
                     "--color", "rgb", "--no-med", "--no-ycocg"}),
            0);
  EXPECT_EQ(run_cli({"info", file}), 0);
  ASSERT_EQ(run_cli(decode_args(file, "0", "100", "50", "130", "70", out)), 0);
  EXPECT_EQ(load_image(out).pixels,
            convert(f.src, ColorFormat::RGB8).crop({100, 50, 130, 70}).pixels);

  // Flags take no value; J2K has neither step.
  EXPECT_EQ(run_cli({"encode", f.png, testutil::temp_path("c2.j2k").string(),
                     "--codec", "j2k", "--no-med"}),
            1);
}

TEST(Cli, InvalidNumbersAreUsageErrors) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_num.png").string();
  for (const char* bad : {"", "12abc", "abc", "1.5", " ", "4294967296",
                          "-2147483649", "99999999999999999999", "+10",
                          " 10", "\t10", "10 ", "-", "--1", "0x10"}) {
    EXPECT_EQ(run_cli(decode_args(f.prmd, "0", bad, "0", "8", "8", out)), 1)
        << "--x '" << bad << "'";
  }
  EXPECT_EQ(run_cli({"encode", f.png, out, "--levels", "5x"}), 1);
  EXPECT_EQ(run_cli({"encode", f.png, out, "--levels", "+3"}), 1);
  EXPECT_EQ(run_cli({"encode", f.png, out, "--threads", " 2"}), 1);
  EXPECT_EQ(run_cli({"encode", f.png, testutil::temp_path("n.j2k").string(),
                     "--codec", "j2k", "--j2k-lossy-psnr", "40dB"}),
            1);
  EXPECT_EQ(run_cli({"encode", f.png, testutil::temp_path("n.j2k").string(),
                     "--codec", "j2k", "--j2k-lossy-psnr", "1e999"}),
            1);
}

TEST(Cli, OutOfRangeRequestsExitWithFour) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_oor.png").string();
  // Huge coordinates: x + w overflows int; must be rejected, not wrap.
  EXPECT_EQ(
      run_cli(decode_args(f.prmd, "0", "2147483647", "0", "16", "16", out)), 4);
  EXPECT_EQ(run_cli(decode_args(f.prmd, "0", "0", "0", "2147483647",
                                "2147483647", out)),
            4);
  EXPECT_EQ(run_cli(decode_args(f.prmd, "0", "-1", "0", "16", "16", out)), 4);
  EXPECT_EQ(run_cli(decode_args(f.prmd, "3", "0", "0", "8", "8", out)), 4);
  EXPECT_EQ(run_cli(decode_args(f.prmd, "-1", "0", "0", "8", "8", out)), 4);
  EXPECT_EQ(run_cli(decode_args(f.prmd, "0", "0", "0", "0", "8", out)), 4);
  // --clamp with no overlap at all is still out of range.
  auto args = decode_args(f.prmd, "0", "2147483647", "2147483647", "16", "16",
                          out);
  args.push_back("--clamp");
  EXPECT_EQ(run_cli(args), 4);
}

TEST(Cli, IoAndFormatErrors) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_err.png").string();
  const std::string missing = testutil::temp_path("missing.prmd").string();
  EXPECT_EQ(run_cli({"info", missing}), 2);
  EXPECT_EQ(run_cli({"encode", missing + ".png", out}), 2);
  EXPECT_EQ(run_cli({"encode", f.png, "/nonexistent-dir/x.prmd"}), 2);

  // A file with the .prmd magic but a broken header.
  std::vector<char> bytes = testutil::read_file(f.prmd);
  bytes[4] = 9;  // version
  const std::string bad = testutil::temp_path("cli_bad.prmd").string();
  testutil::write_file(bad, bytes);
  EXPECT_EQ(run_cli({"info", bad}), 3);
  EXPECT_EQ(run_cli(decode_args(bad, "0", "0", "0", "8", "8", out)), 3);

  // Truncated data.
  bytes = testutil::read_file(f.prmd);
  bytes.pop_back();
  testutil::write_file(bad, bytes);
  EXPECT_EQ(run_cli(decode_args(bad, "0", "0", "0", "8", "8", out)), 3);
}

TEST(Cli, ThreadsOption) {
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_threads.prmd").string();
  for (const char* bad : {"-1", "abc", "1.5", "", "4x", "99999999999"}) {
    EXPECT_EQ(run_cli({"encode", f.png, out, "--threads", bad}), 1)
        << "--threads '" << bad << "'";
  }
  EXPECT_EQ(run_cli({"encode", f.png, out, "--threads"}), 1);  // no value

  // Every accepted count writes the file of --threads 1.
  const std::string serial = testutil::temp_path("cli_t1.prmd").string();
  ASSERT_EQ(run_cli({"encode", f.png, serial, "--levels", "3", "--mode",
                     "cdf53", "--cblk", "32", "--tile", "64", "--threads",
                     "1"}),
            0);
  const auto expected = testutil::read_file(serial);
  ASSERT_FALSE(expected.empty());
  for (const char* n : {"0", "2", "3", "16", "1000"}) {
    ASSERT_EQ(run_cli({"encode", f.png, out, "--levels", "3", "--mode",
                       "cdf53", "--cblk", "32", "--tile", "64", "--threads",
                       n}),
              0)
        << n;
    EXPECT_EQ(testutil::read_file(out), expected) << "--threads " << n;
  }
  // J2K takes it too.
  EXPECT_EQ(run_cli({"encode", f.png, testutil::temp_path("t.j2k").string(),
                     "--codec", "j2k", "--levels", "3", "--threads", "4"}),
            0);
}

namespace {

// The lines `prmd encode` printed (stdout and stderr of the last run_cli).
std::vector<std::string> log_lines() {
  const std::vector<char> b =
      testutil::read_file(testutil::temp_path("cli.log").string());
  std::vector<std::string> lines;
  std::string cur;
  for (const char c : b) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    lines.push_back(cur);
  }
  return lines;
}

// Each line's label: the report's labels are padded to 16 columns.
std::vector<std::string> labels(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  for (const std::string& l : lines) {
    out.push_back(l.substr(0, 16));
  }
  return out;
}

}  // namespace

TEST(Cli, SerialEncodePrintsWhatItPrintedBeforeThreading) {
  // --threads 1 prints exactly the report of the encoder before threading:
  // no "threads" line. Labels and order are those of tools/prmd_cli.cpp on
  // main (input, output, sizes, per-level bytes, encode time).
  const CliFiles& f = files();
  const std::string out = testutil::temp_path("cli_serial.prmd").string();
  ASSERT_EQ(run_cli({"encode", f.png, out, "--levels", "3", "--threads", "1"}),
            0);
  const std::vector<std::string> serial = log_lines();
  const std::vector<std::string> expected_labels = {
      "input           ",
      "output          ",
      "file bytes      ",
      "raw level 0     ",
      "raw pyramid     ",
      "  level 0 bytes ",
      "  level 1 bytes ",
      "  level 2 bytes ",
      "encode time     ",
  };
  EXPECT_EQ(labels(serial), expected_labels);
  ASSERT_EQ(serial.size(), expected_labels.size());
  EXPECT_EQ(serial[0], "input           " + f.png + "  300x200 gray");
  EXPECT_EQ(serial[1], "output          " + out);

  // A small image under the automatic default is serial too: same report.
  ASSERT_EQ(run_cli({"encode", f.png, out, "--levels", "3"}), 0);
  std::vector<std::string> automatic = log_lines();
  ASSERT_EQ(automatic.size(), serial.size());
  // Everything but the time is identical.
  for (size_t i = 0; i + 1 < serial.size(); ++i) {
    EXPECT_EQ(automatic[i], serial[i]) << i;
  }

  // A multithreaded encode adds one line, and only that.
  ASSERT_EQ(run_cli({"encode", f.png, out, "--levels", "3", "--cblk", "32",
                     "--tile", "64", "--threads", "3"}),
            0);
  const std::vector<std::string> mt = log_lines();
  ASSERT_FALSE(mt.empty());
  EXPECT_EQ(mt.back(), "threads         3");
  std::vector<std::string> mt_labels = labels(mt);
  mt_labels.pop_back();
  EXPECT_EQ(mt_labels, expected_labels);
}

TEST(Cli, InputLineNamesTheStoredFormatWhenItDiffers) {
  const CliFiles& f = files();
  const std::string rgb_png = testutil::temp_path("cli_rgb_src.png").string();
  save_png(rgb_png, convert(f.src, ColorFormat::RGB8));
  const std::string out = testutil::temp_path("cli_stored.prmd").string();

  // An RGB input stored as gray (the default) says so.
  ASSERT_EQ(run_cli({"encode", rgb_png, out, "--levels", "3"}), 0);
  std::vector<std::string> lines = log_lines();
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines[0],
            "input           " + rgb_png + "  300x200 rgb (stored as gray)");

  // Stored as it came: no note.
  ASSERT_EQ(
      run_cli({"encode", rgb_png, out, "--levels", "3", "--color", "rgb"}), 0);
  lines = log_lines();
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(lines[0], "input           " + rgb_png + "  300x200 rgb");
}
