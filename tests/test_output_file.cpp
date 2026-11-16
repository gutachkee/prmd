// Encoder output files (include/prmd/internal/output_file.h): written to a
// temporary file and renamed over the target only when complete, so a
// failed encode never truncates or half-writes an existing file; fopen-like
// permissions, symbolic links followed, non-regular targets written
// directly. Write failures part-way through an encode:
// tests/test_threads.cpp, ThreadsErrors.WriteFailureLeavesTheExistingFileIntact.

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "prmd/encoder.h"
#include "prmd/errors.h"
#include "prmd/internal/output_file.h"
#include "reference.h"
#include "test_util.h"

using namespace prmd;
using prmd::internal::OutputFile;
namespace fs = std::filesystem;

namespace {

// A fresh empty directory under the test directory.
fs::path fresh_dir(const std::string& name) {
  const fs::path d = testutil::temp_path(name);
  std::error_code ec;
  fs::permissions(d, fs::perms::owner_all, fs::perm_options::add, ec);
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

std::vector<std::string> names_in(const fs::path& d) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(d)) {
    out.push_back(e.path().filename().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

void write_all(OutputFile& f, const std::string& text) {
  ASSERT_EQ(std::fwrite(text.data(), 1, text.size(), f.stream()), text.size());
}

std::string read_text(const fs::path& p) {
  const std::vector<char> b = testutil::read_file(p.string());
  return std::string(b.begin(), b.end());
}

unsigned mode_of(const fs::path& p) {
  struct stat st;
  EXPECT_EQ(::stat(p.c_str(), &st), 0);
  return unsigned(st.st_mode & 07777);
}

struct Umask {
  explicit Umask(mode_t m) : old(::umask(m)) {}
  ~Umask() { ::umask(old); }
  mode_t old;
};

bool is_root() {
  return ::geteuid() == 0;
}

}  // namespace

TEST(OutputFile, WritesThroughATemporaryFileRenamedOnCommit) {
  const fs::path d = fresh_dir("of_basic");
  const fs::path target = d / "out.bin";
  OutputFile f(target.string());
  const std::string tmp = f.write_path();
  EXPECT_EQ(tmp.rfind(target.string() + ".tmp." + std::to_string(::getpid()) +
                          ".",
                      0),
            0u)
      << tmp;
  EXPECT_TRUE(fs::exists(tmp));
  EXPECT_FALSE(fs::exists(target));  // nothing at the target yet
  write_all(f, "hello");
  EXPECT_EQ(f.commit(), 5u);
  EXPECT_EQ(read_text(target), "hello");
  EXPECT_EQ(names_in(d), std::vector<std::string>{"out.bin"});
}

TEST(OutputFile, AbandonedOrFailedWritesLeaveTheTargetUntouched) {
  const fs::path d = fresh_dir("of_fail");
  const fs::path target = d / "keep.bin";
  testutil::write_file(target.string(), {'o', 'l', 'd'});
  {
    OutputFile f(target.string());
    write_all(f, "new contents");
    // Destroyed without commit (an exception in the encoder).
  }
  EXPECT_EQ(read_text(target), "old");
  EXPECT_EQ(names_in(d), std::vector<std::string>{"keep.bin"});

  OutputFile f(target.string());
  write_all(f, "new contents");
  f.fail();  // a short write
  try {
    f.commit();
    ADD_FAILURE() << "commit after fail() succeeded";
  } catch (const IoError& e) {
    EXPECT_EQ(std::string(e.what()),
              "write failed for '" + target.string() + "'");
  }
  EXPECT_EQ(read_text(target), "old");
  EXPECT_EQ(names_in(d), std::vector<std::string>{"keep.bin"});
}

TEST(OutputFile, NewFilesGetUmaskPermissionsReplacedFilesKeepTheirs) {
  const Umask mask(022);
  const fs::path d = fresh_dir("of_mode");
  const fs::path fresh = d / "new.bin";
  {
    OutputFile f(fresh.string());
    write_all(f, "x");
    f.commit();
  }
  EXPECT_EQ(mode_of(fresh), 0644u);  // as fopen(path, "wb")

  const fs::path kept = d / "kept.bin";
  testutil::write_file(kept.string(), {'o'});
  ASSERT_EQ(::chmod(kept.c_str(), 0640), 0);
  {
    OutputFile f(kept.string());
    write_all(f, "replaced");
    f.commit();
  }
  EXPECT_EQ(read_text(kept), "replaced");
  EXPECT_EQ(mode_of(kept), 0640u);
}

TEST(OutputFile, SymbolicLinksAreFollowed) {
  const fs::path d = fresh_dir("of_link");
  const fs::path real = d / "real.bin";
  const fs::path link = d / "link.bin";
  testutil::write_file(real.string(), {'o', 'l', 'd'});
  fs::create_symlink("real.bin", link);  // relative to the link's directory
  {
    OutputFile f(link.string());
    write_all(f, "through the link");
    f.commit();
  }
  EXPECT_TRUE(fs::is_symlink(link));
  EXPECT_EQ(read_text(real), "through the link");

  // A dangling link: the file it names is created, as fopen would.
  const fs::path dangling = d / "dangling.bin";
  fs::create_symlink(d / "made.bin", dangling);
  {
    OutputFile f(dangling.string());
    write_all(f, "made");
    f.commit();
  }
  EXPECT_TRUE(fs::is_symlink(dangling));
  EXPECT_EQ(read_text(d / "made.bin"), "made");
  EXPECT_EQ(names_in(d), (std::vector<std::string>{"dangling.bin", "link.bin",
                                                   "made.bin", "real.bin"}));
}

TEST(OutputFile, MissingOrUnwritableDirectoryIsAnErrorWithNoFileLeft) {
  const fs::path d = fresh_dir("of_dirs");
  const fs::path missing = d / "no_such_dir" / "x.prmd";
  try {
    OutputFile f(missing.string());
    ADD_FAILURE() << "created a file in a missing directory";
  } catch (const IoError& e) {
    EXPECT_EQ(std::string(e.what()),
              "cannot create '" + missing.string() + "'");
  }
  EXPECT_TRUE(names_in(d).empty());

  if (is_root()) {
    GTEST_SKIP() << "root ignores directory permissions";
  }
  const fs::path ro = d / "ro";
  fs::create_directories(ro);
  testutil::write_file((ro / "old.prmd").string(), {'o', 'l', 'd'});
  ASSERT_EQ(::chmod(ro.c_str(), 0555), 0);
  for (const char* name : {"new.prmd", "old.prmd"}) {
    EXPECT_THROW(OutputFile((ro / name).string()), IoError) << name;
  }
  // Through the encoder too, both codecs.
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  EncodeOptions o;
  o.levels = 3;
  EXPECT_THROW(encode_file(img, (ro / "old.prmd").string(), Codec::Prmd, o),
               IoError);
  EXPECT_THROW(encode_file(img, (ro / "old.j2k").string(), Codec::J2K, o),
               IoError);
  EXPECT_THROW(encode_file(img, missing.string(), Codec::Prmd, o), IoError);
  EXPECT_EQ(names_in(ro), std::vector<std::string>{"old.prmd"});
  EXPECT_EQ(read_text(ro / "old.prmd"), "old");
  ASSERT_EQ(::chmod(ro.c_str(), 0755), 0);
}

TEST(OutputFile, ReadOnlyFileIsNotReplaced) {
  if (is_root()) {
    GTEST_SKIP() << "root may write read-only files";
  }
  // fopen("wb") is refused for a file the caller may not write; replacing
  // it by rename() in a writable directory would not be.
  const fs::path d = fresh_dir("of_readonly");
  const fs::path target = d / "ro.prmd";
  testutil::write_file(target.string(), {'o', 'l', 'd'});
  ASSERT_EQ(::chmod(target.c_str(), 0444), 0);
  EXPECT_THROW(OutputFile(target.string()), IoError);
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  EncodeOptions o;
  o.levels = 3;
  EXPECT_THROW(encode_file(img, target.string(), Codec::Prmd, o), IoError);
  EXPECT_EQ(read_text(target), "old");
  EXPECT_EQ(names_in(d), std::vector<std::string>{"ro.prmd"});
}

TEST(OutputFile, NonRegularTargetsAreWrittenDirectly) {
  // /dev/null cannot (and must not) be replaced: it is written as fopen
  // would, and stays a device.
  if (!fs::exists("/dev/null")) {
    GTEST_SKIP();
  }
  {
    OutputFile f("/dev/null");
    EXPECT_EQ(f.write_path(), "/dev/null");
    write_all(f, "discarded");
    f.commit();
  }
  EXPECT_TRUE(fs::is_character_file("/dev/null"));
  const Image img = ref::synthetic_image(300, 200, ColorFormat::RGB8, 3);
  EncodeOptions o;
  o.levels = 3;
  o.color = ColorFormat::RGB8;
  EXPECT_GT(encode_file(img, "/dev/null", Codec::Prmd, o).file_bytes, 0u);
  EXPECT_NO_THROW(encode_file(img, "/dev/null", Codec::J2K, o));
  EXPECT_TRUE(fs::is_character_file("/dev/null"));

  // A directory is an error, as for fopen.
  const fs::path d = fresh_dir("of_isdir");
  EXPECT_THROW(OutputFile(d.string()), IoError);
  EXPECT_TRUE(fs::is_directory(d));
}

TEST(OutputFile, EncoderReportsTheBytesItWrote) {
  const fs::path d = fresh_dir("of_enc");
  const Image img = ref::synthetic_image(300, 200, ColorFormat::RGB8, 3);
  EncodeOptions o;
  o.levels = 3;
  o.color = ColorFormat::RGB8;
  for (const Codec codec : {Codec::Prmd, Codec::J2K}) {
    const fs::path p = d / (codec == Codec::Prmd ? "a.prmd" : "a.j2k");
    testutil::write_file(p.string(), std::vector<char>(100000, 'x'));
    const EncodeReport r = encode_file(img, p.string(), codec, o);
    EXPECT_EQ(fs::file_size(p), r.file_bytes);
    EXPECT_NE(fs::file_size(p), 100000u);
  }
  EXPECT_EQ(names_in(d), (std::vector<std::string>{"a.j2k", "a.prmd"}));
}
