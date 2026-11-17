// Strict integer arguments of the tools (tools/tool_args.h, also used by the
// prmd CLI's Args::num): the whole string must be a decimal integer, with a
// '-' only where negatives are allowed; and prmdbench / prmdpipeline exit
// with an error message, not std::terminate, on a missing input image.

#include <gtest/gtest.h>

#include <sys/wait.h>

#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_util.h"
#include "tool_args.h"

using prmd::tools::is_decimal_integer;
using prmd::tools::parse_int;
using prmd::tools::parse_int_list;

TEST(ToolArgs, OnlyDigitsWithAnOptionalMinus) {
  for (const char* ok : {"0", "7", "0012", "123456789"}) {
    EXPECT_TRUE(is_decimal_integer(ok, false)) << ok;
    EXPECT_TRUE(is_decimal_integer(ok, true)) << ok;
  }
  EXPECT_TRUE(is_decimal_integer("-5", true));
  EXPECT_FALSE(is_decimal_integer("-5", false));
  for (const char* bad : {"", "-", "+5", " 5", "5 ", "\t5", "\n5", "5x",
                          "1.5", "0x10", "--5", "-+5", "+-5", "1e3", "٣"}) {
    EXPECT_FALSE(is_decimal_integer(bad, false)) << "'" << bad << "'";
    EXPECT_FALSE(is_decimal_integer(bad, true)) << "'" << bad << "'";
  }
}

TEST(ToolArgs, ParseIntIsStrictAndRangeChecked) {
  long long v = -1;
  EXPECT_TRUE(parse_int("42", 0, 100, &v));
  EXPECT_EQ(v, 42);
  EXPECT_TRUE(parse_int("0", 0, 100, &v));
  EXPECT_EQ(v, 0);
  for (const char* bad : {"", " 42", "+42", "42 ", "4 2", "42abc", "-0",
                          "-1", "101", "99999999999999999999"}) {
    v = 7;
    EXPECT_FALSE(parse_int(bad, 0, 100, &v)) << "'" << bad << "'";
    EXPECT_EQ(v, 7) << "unchanged on failure";
  }
  // A '-' only when the range has negatives.
  EXPECT_TRUE(parse_int("-3", -10, 10, &v));
  EXPECT_EQ(v, -3);
  EXPECT_FALSE(parse_int("-11", -10, 10, &v));
  EXPECT_FALSE(parse_int("+3", -10, 10, &v));
  EXPECT_FALSE(parse_int(" -3", -10, 10, &v));
  EXPECT_TRUE(parse_int(std::to_string(LLONG_MAX), 0, LLONG_MAX, &v));
  EXPECT_FALSE(parse_int("9223372036854775808", 0, LLONG_MAX, &v));
}

TEST(ToolArgs, ParseIntListRejectsBadItems) {
  std::vector<int> v;
  EXPECT_TRUE(parse_int_list("1,2,4,8,0", 0, INT_MAX, &v));
  EXPECT_EQ(v, (std::vector<int>{1, 2, 4, 8, 0}));
  for (const char* bad : {"", ",", "1,", ",1", "1,,2", "1, 2", "1,+2",
                          "1,-2", "a", "1,2x"}) {
    EXPECT_FALSE(parse_int_list(bad, 0, INT_MAX, &v)) << "'" << bad << "'";
  }
}

namespace {

// Runs `cmd args...` with its output in a log; the exit code, or -1 if it
// did not exit normally (std::terminate aborts).
int run_tool(const std::string& exe, const std::vector<std::string>& args) {
  std::string cmd =
      "ASAN_OPTIONS=exitcode=86 UBSAN_OPTIONS=exitcode=86 '" + exe + "'";
  for (const std::string& a : args) {
    cmd += " '" + a + "'";
  }
  cmd += " > '" + testutil::temp_path("tool.log").string() + "' 2>&1";
  const int status = std::system(cmd.c_str());
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string tool_log() {
  const std::vector<char> b =
      testutil::read_file(testutil::temp_path("tool.log").string());
  return std::string(b.begin(), b.end());
}

}  // namespace

TEST(ToolArgs, BenchAndPipelineReportAMissingImage) {
  const std::string missing = testutil::temp_path("missing.jpg").string();
  const std::string out = testutil::temp_path("bench.md").string();
  EXPECT_EQ(run_tool(PRMD_BENCH_PATH, {missing, "--out", out}), 2);
  EXPECT_NE(tool_log().find("prmdbench: "), std::string::npos) << tool_log();
  EXPECT_EQ(tool_log().find("terminate"), std::string::npos) << tool_log();
  const std::string file = testutil::temp_path("pipe.prmd").string();
  EXPECT_EQ(run_tool(PRMD_PIPELINE_PATH, {missing, "--file", file}), 2);
  EXPECT_NE(tool_log().find("prmdpipeline: "), std::string::npos)
      << tool_log();
  // Strict numbers there too: usage errors.
  EXPECT_EQ(run_tool(PRMD_BENCH_PATH, {missing, "--iters", "+5"}), 1);
  EXPECT_EQ(run_tool(PRMD_BENCH_PATH, {missing, "--threads", " 2"}), 1);
  EXPECT_EQ(run_tool(PRMD_PIPELINE_PATH, {missing, "--runs", "+1"}), 1);
}
