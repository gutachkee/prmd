// Encoder output files (include/prmd/internal/output_file.h): written to a
// temporary file and renamed over the target only when complete, so a
// failed encode never truncates or half-writes an existing file; fopen-like
// permissions, symbolic links followed (loops refused), non-regular targets
// and writable files that cannot be replaced written in place, temporary
// names that fit NAME_MAX, the empty path refused before any work. Write
// failures part-way through an encode: tests/test_threads.cpp,
// ThreadsErrors.WriteFailureLeavesTheExistingFileIntact.

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"
#include "prmd/errors.h"
#include "prmd/internal/blockcoder.h"
#include "prmd/internal/output_file.h"
#include "prmd/internal/parallel.h"
#include "prmd/internal/prmd_codec.h"
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
  // A new file in a read-only directory cannot be created at all.
  const fs::path ro = d / "ro";
  fs::create_directories(ro);
  testutil::write_file((ro / "old.prmd").string(), {'o', 'l', 'd'});
  ASSERT_EQ(::chmod(ro.c_str(), 0555), 0);
  try {
    OutputFile f((ro / "new.prmd").string());
    ADD_FAILURE() << "created a file in a read-only directory";
  } catch (const IoError& e) {
    EXPECT_EQ(std::string(e.what()),
              "cannot create '" + (ro / "new.prmd").string() + "'");
  }
  // Through the encoder too, both codecs.
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  EncodeOptions o;
  o.levels = 3;
  EXPECT_THROW(encode_file(img, (ro / "new.prmd").string(), Codec::Prmd, o),
               IoError);
  EXPECT_THROW(encode_file(img, (ro / "new.j2k").string(), Codec::J2K, o),
               IoError);
  EXPECT_THROW(encode_file(img, missing.string(), Codec::Prmd, o), IoError);
  EXPECT_EQ(names_in(ro), std::vector<std::string>{"old.prmd"});
  EXPECT_EQ(read_text(ro / "old.prmd"), "old");
  ASSERT_EQ(::chmod(ro.c_str(), 0755), 0);
}

TEST(OutputFile, WritableFileInAReadOnlyDirectoryIsWrittenInPlace) {
  if (is_root()) {
    GTEST_SKIP() << "root ignores directory permissions";
  }
  // No temporary file can be made next to it, but fopen("wb") could write
  // it: it is written in place, without the replace-on-success guarantee.
  const fs::path ro = fresh_dir("of_ro_inplace");
  const fs::path target = ro / "old.prmd";
  testutil::write_file(target.string(), {'o', 'l', 'd'});
  ASSERT_EQ(::chmod(target.c_str(), 0640), 0);
  testutil::write_file((ro / "old.j2k").string(), {'o', 'l', 'd'});
  ASSERT_EQ(::chmod(ro.c_str(), 0555), 0);
  struct Restore {
    fs::path d;
    ~Restore() { ::chmod(d.c_str(), 0755); }
  } restore{ro};
  {
    OutputFile f(target.string());
    EXPECT_EQ(f.write_path(), target.string());
    write_all(f, "new contents");
    EXPECT_EQ(f.commit(), 12u);
  }
  EXPECT_EQ(read_text(target), "new contents");
  EXPECT_EQ(mode_of(target), 0640u);  // the same file
  // A failed write leaves what fopen("wb") would: the file truncated or
  // partial, and no other file.
  {
    OutputFile f(target.string());
    write_all(f, "part");
    f.fail();
    EXPECT_THROW(f.commit(), IoError);
  }
  EXPECT_TRUE(fs::is_regular_file(target));

  // Through both encoders: the files decode.
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  EncodeOptions o;
  o.levels = 3;
  for (const Codec codec : {Codec::Prmd, Codec::J2K}) {
    const fs::path p = ro / (codec == Codec::Prmd ? "old.prmd" : "old.j2k");
    const EncodeReport r = encode_file(img, p.string(), codec, o);
    EXPECT_EQ(fs::file_size(p), r.file_bytes);
    EXPECT_EQ(open_decoder(p.string())->decode(0, {0, 0, 64, 48}).image.pixels,
              img.pixels);
  }
  EXPECT_EQ(names_in(ro), (std::vector<std::string>{"old.j2k", "old.prmd"}));
}

TEST(OutputFile, LongNamesGetAShortenedTemporaryName) {
  // "<name>.tmp.<pid>.<16 hex>" would exceed NAME_MAX (255) for a 250-byte
  // name: the temporary name keeps a prefix of it, in the same directory.
  const fs::path d = fresh_dir("of_long");
  const std::string name(250, 'n');
  const fs::path target = d / name;
  for (int round = 0; round < 2; ++round) {  // a new file, then a replaced one
    OutputFile f(target.string());
    const fs::path tmp = f.write_path();
    EXPECT_NE(tmp, target);
    EXPECT_EQ(tmp.parent_path(), d);
    EXPECT_LE(tmp.filename().string().size(), 255u);
    EXPECT_EQ(tmp.filename().string().rfind(name.substr(0, 200), 0), 0u);
    write_all(f, round == 0 ? "first" : "second");
    f.commit();
    EXPECT_EQ(names_in(d), std::vector<std::string>{name});
  }
  EXPECT_EQ(read_text(target), "second");
  // The longest name works too, and through the encoder.
  const fs::path longest = d / std::string(255, 'm');
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  EncodeOptions o;
  o.levels = 3;
  const EncodeReport r = encode_file(img, longest.string(), Codec::Prmd, o);
  EXPECT_EQ(fs::file_size(longest), r.file_bytes);
  EXPECT_EQ(names_in(d).size(), 2u);
}

TEST(OutputFile, SymbolicLinkLoopIsAnError) {
  // fopen fails with ELOOP; the link must not be replaced by a file.
  const fs::path d = fresh_dir("of_loop");
  fs::create_symlink("b", d / "a");
  fs::create_symlink("a", d / "b");
  fs::create_symlink("self", d / "self");
  for (const char* name : {"a", "self"}) {
    const fs::path p = d / name;
    try {
      OutputFile f(p.string());
      ADD_FAILURE() << "opened a symbolic link loop: " << name;
    } catch (const IoError& e) {
      EXPECT_EQ(std::string(e.what()), "cannot create '" + p.string() + "'");
    }
  }
  const Image img = ref::synthetic_image(64, 48, ColorFormat::Gray8, 3);
  EncodeOptions o;
  o.levels = 3;
  EXPECT_THROW(encode_file(img, (d / "a").string(), Codec::Prmd, o), IoError);
  EXPECT_THROW(encode_file(img, (d / "self").string(), Codec::J2K, o), IoError);
  EXPECT_TRUE(fs::is_symlink(d / "a"));
  EXPECT_TRUE(fs::is_symlink(d / "b"));
  EXPECT_TRUE(fs::is_symlink(d / "self"));
  EXPECT_EQ(names_in(d), (std::vector<std::string>{"a", "b", "self"}));

  // A chain of 40 links (the kernel's limit) still reaches its file.
  testutil::write_file((d / "end").string(), {'o'});
  std::string next = "end";
  for (int i = 0; i < 40; ++i) {
    const std::string link = "l" + std::to_string(i);
    fs::create_symlink(next, d / link);
    next = link;
  }
  {
    OutputFile f((d / next).string());
    write_all(f, "via 40 links");
    f.commit();
  }
  EXPECT_EQ(read_text(d / "end"), "via 40 links");
  EXPECT_TRUE(fs::is_symlink(d / next));
}

namespace {

// zstd coder that counts the blocks it codes.
class CountingCoder final : public internal::BlockCoder {
 public:
  CountingCoder() : inner_(internal::make_block_coder(Entropy::Zstd)) {}
  Entropy id() const override { return Entropy::Zstd; }
  void encode(const internal::BlockPlanes& in,
              std::vector<uint8_t>& out) const override {
    blocks.fetch_add(1);
    inner_->encode(in, out);
  }
  void decode(const uint8_t* d,
              size_t n,
              internal::BlockPlanes& io) const override {
    inner_->decode(d, n, io);
  }
  mutable std::atomic<int> blocks{0};

 private:
  std::unique_ptr<internal::BlockCoder> inner_;
};

std::atomic<int> g_stripes{0};
void count_stripe(int, int) {
  g_stripes.fetch_add(1);
}

}  // namespace

TEST(OutputFile, EmptyPathFailsBeforeAnyWork) {
  try {
    OutputFile f("");
    ADD_FAILURE() << "opened ''";
  } catch (const IoError& e) {
    EXPECT_EQ(std::string(e.what()), "cannot create ''");
  }
  // The encoders check the path before converting or coding anything.
  const Image img = ref::synthetic_image(600, 400, ColorFormat::RGB8, 3);
  EncodeOptions o;
  o.levels = 3;
  o.color = ColorFormat::RGB8;
  struct Reset {
    ~Reset() { internal::test_hooks::set_row_stripe_observer(nullptr); }
  } reset;
  g_stripes = 0;
  internal::test_hooks::set_row_stripe_observer(count_stripe);
  for (const int threads : {1, 4}) {
    o.threads = threads;
    const CountingCoder coder;
    try {
      internal::encode_prmd(img, "", o, &coder);
      ADD_FAILURE() << "encoded to ''";
    } catch (const IoError& e) {
      EXPECT_EQ(std::string(e.what()), "cannot create ''");
    }
    EXPECT_EQ(coder.blocks.load(), 0) << threads;
    EXPECT_EQ(g_stripes.load(), 0) << threads;
    EXPECT_THROW(encode_file(img, "", Codec::Prmd, o), IoError);
    EXPECT_THROW(encode_file(img, "", Codec::J2K, o), IoError);
  }
  // Invalid options are still reported first, as before.
  o.levels = 0;
  EXPECT_THROW(encode_file(img, "", Codec::Prmd, o), std::invalid_argument);
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
