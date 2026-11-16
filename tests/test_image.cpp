#include <gtest/gtest.h>

#include <limits>

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

#include <jpeglib.h>

#include "prmd/errors.h"
#include "prmd/image.h"
#include "prmd/image_io.h"
#include "test_util.h"

using namespace prmd;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& name) {
  return testutil::temp_path(name);
}

Image random_image(int w, int h, ColorFormat f, unsigned seed) {
  Image img(w, h, f);
  std::mt19937 rng(seed);
  for (auto& p : img.pixels) {
    p = uint8_t(rng());
  }
  return img;
}

// Writes a JPEG with libjpeg so the loader can be tested without fixture files.
void write_jpeg(const fs::path& path, const Image& img, int quality) {
  FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  jpeg_compress_struct cinfo;
  jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, f);
  cinfo.image_width = JDIMENSION(img.width);
  cinfo.image_height = JDIMENSION(img.height);
  cinfo.input_components = img.channels();
  cinfo.in_color_space =
      img.format == ColorFormat::Gray8 ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<uint8_t*>(img.row(int(cinfo.next_scanline)));
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  std::fclose(f);
}

}  // namespace

TEST(Rect, Intersect) {
  EXPECT_EQ(intersect({0, 0, 10, 10}, {5, 5, 10, 10}), (Rect{5, 5, 5, 5}));
  EXPECT_TRUE(intersect({0, 0, 10, 10}, {10, 0, 5, 5}).empty());
  EXPECT_EQ(intersect({-5, -5, 10, 10}, {0, 0, 100, 100}), (Rect{0, 0, 5, 5}));
  EXPECT_EQ((Rect{3, 4, 5, 6}).right(), 8);
  EXPECT_EQ((Rect{3, 4, 5, 6}).bottom(), 10);
}

TEST(Rect, IntersectNearIntMaxDoesNotOverflow) {
  const int big = std::numeric_limits<int>::max();
  EXPECT_TRUE(intersect({big - 5, 0, 100, 1}, {0, 0, 1000, 1000}).empty());
  EXPECT_EQ(intersect({big - 5, 0, 100, 1}, {big - 10, 0, 8, 1}),
            (Rect{big - 5, 0, 3, 1}));
}

TEST(ColorFormat, Info) {
  EXPECT_EQ(channels(ColorFormat::Gray8), 1);
  EXPECT_EQ(channels(ColorFormat::RGB8), 3);
  EXPECT_STREQ(info(ColorFormat::RGB8).name, "rgb");
  EXPECT_TRUE(is_valid_color_format(0));
  EXPECT_TRUE(is_valid_color_format(1));
  EXPECT_FALSE(is_valid_color_format(3));
  EXPECT_THROW(info(ColorFormat(7)), std::invalid_argument);
}

TEST(Image, LayoutAndCrop) {
  Image img = random_image(7, 5, ColorFormat::RGB8, 1);
  EXPECT_EQ(img.stride(), 21u);
  const Image c = img.crop({2, 1, 3, 2});
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 3; ++x) {
      for (int ch = 0; ch < 3; ++ch) {
        EXPECT_EQ(c.at(x, y, ch), img.at(x + 2, y + 1, ch));
      }
    }
  }
  EXPECT_THROW(img.crop({5, 0, 3, 1}), std::out_of_range);
  EXPECT_THROW(img.crop({0, 0, 0, 1}), std::out_of_range);
  EXPECT_THROW(img.crop({-1, 0, 1, 1}), std::out_of_range);
  const int big = std::numeric_limits<int>::max();
  EXPECT_THROW(img.crop({big - 1, 0, 3, 1}), std::out_of_range);
  EXPECT_THROW(img.crop({0, big - 1, 1, 3}), std::out_of_range);
}

TEST(Image, Convert) {
  Image rgb(2, 1, ColorFormat::RGB8);
  const uint8_t px[] = {255, 255, 255, 10, 200, 30};
  std::copy(px, px + 6, rgb.pixels.begin());
  const Image g = convert(rgb, ColorFormat::Gray8);
  EXPECT_EQ(g.at(0, 0), 255);
  EXPECT_EQ(g.at(1, 0), (77 * 10 + 150 * 200 + 29 * 30 + 128) >> 8);
  const Image back = convert(g, ColorFormat::RGB8);
  for (int c = 0; c < 3; ++c) {
    EXPECT_EQ(back.at(1, 0, c), g.at(1, 0));
  }
}

TEST(ImageIo, PngRoundTripGrayAndRgb) {
  for (auto f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
    const Image img = random_image(33, 17, f, 7);
    const auto p = temp_path(std::string("rt_") + info(f).name + ".png");
    save_png(p.string(), img);
    const Image back = load_image(p.string());
    EXPECT_EQ(back.format, f);
    EXPECT_EQ(back.width, 33);
    EXPECT_EQ(back.height, 17);
    EXPECT_EQ(back.pixels, img.pixels);
  }
}

TEST(ImageIo, JpegLoadGrayAndRgb) {
  for (auto f : {ColorFormat::Gray8, ColorFormat::RGB8}) {
    Image img(64, 48, f);
    for (int y = 0; y < img.height; ++y) {
      for (int x = 0; x < img.width * img.channels(); ++x) {
        img.row(y)[x] = uint8_t(x + 2 * y);
      }
    }
    const auto p = temp_path(std::string("ld_") + info(f).name + ".jpg");
    write_jpeg(p, img, 100);
    const Image back = load_image(p.string());
    EXPECT_EQ(back.format, f);
    EXPECT_EQ(back.width, 64);
    EXPECT_EQ(back.height, 48);
    // Lossy codec: values must be close, not identical.
    long err = 0;
    for (size_t i = 0; i < img.pixels.size(); ++i) {
      err += std::abs(int(back.pixels[i]) - int(img.pixels[i]));
    }
    EXPECT_LT(double(err) / double(img.pixels.size()), 4.0);
  }
}

TEST(ImageIo, Errors) {
  EXPECT_THROW(load_image("/nonexistent/x.png"), IoError);
  const auto p = temp_path("garbage.bin");
  FILE* f = std::fopen(p.c_str(), "wb");
  std::fputs("not an image at all", f);
  std::fclose(f);
  EXPECT_THROW(load_image(p.string()), IoError);
  // Truncated JPEG
  const auto j = temp_path("trunc.jpg");
  write_jpeg(j, random_image(32, 32, ColorFormat::Gray8, 3), 90);
  fs::resize_file(j, 200);
  EXPECT_THROW(load_image(j.string()), IoError);
}
