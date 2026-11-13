#include "prmd/image_io.h"

#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>

#include <jpeglib.h>
#include <png.h>

#include "prmd/errors.h"

namespace prmd {

namespace {

struct FileCloser {
  void operator()(FILE* f) const {
    if (f) std::fclose(f);
  }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr open_file(const std::string& path, const char* mode) {
  FilePtr f(std::fopen(path.c_str(), mode));
  if (!f) throw IoError("cannot open '" + path + "': " + std::strerror(errno));
  return f;
}

// ---------------------------------------------------------------- JPEG

struct JpegErr {
  jpeg_error_mgr mgr;
  std::jmp_buf jump;
  char msg[JMSG_LENGTH_MAX];
};

void jpeg_error_exit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JpegErr*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->msg);
  std::longjmp(err->jump, 1);
}

Image load_jpeg(FILE* f, const std::string& path) {
  jpeg_decompress_struct cinfo;
  JpegErr err;
  cinfo.err = jpeg_std_error(&err.mgr);
  err.mgr.error_exit = jpeg_error_exit;
  err.msg[0] = 0;
  // Everything touched after setjmp and read after longjmp lives outside this frame's
  // registers (the Image is heap-backed through unique_ptr).
  auto img = std::make_unique<Image>();
  if (setjmp(err.jump)) {
    jpeg_destroy_decompress(&cinfo);
    throw IoError("JPEG decode failed for '" + path + "': " + err.msg);
  }
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, f);
  jpeg_read_header(&cinfo, TRUE);
  const bool gray = cinfo.jpeg_color_space == JCS_GRAYSCALE;
  cinfo.out_color_space = gray ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_start_decompress(&cinfo);
  *img = Image(int(cinfo.output_width), int(cinfo.output_height),
               gray ? ColorFormat::Gray8 : ColorFormat::RGB8);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW rowp = img->row(int(cinfo.output_scanline));
    jpeg_read_scanlines(&cinfo, &rowp, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return std::move(*img);
}

// ---------------------------------------------------------------- PNG

struct PngErr {
  char msg[256] = {0};
};

void png_error_fn(png_structp png, png_const_charp msg) {
  auto* e = static_cast<PngErr*>(png_get_error_ptr(png));
  if (e) std::snprintf(e->msg, sizeof(e->msg), "%s", msg);
  png_longjmp(png, 1);
}
void png_warn_fn(png_structp, png_const_charp) {}

Image load_png(FILE* f, const std::string& path) {
  PngErr perr;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &perr, png_error_fn, png_warn_fn);
  if (!png) throw IoError("png_create_read_struct failed");
  png_infop info = png_create_info_struct(png);
  auto img = std::make_unique<Image>();
  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw IoError("PNG decode failed for '" + path + "': " + perr.msg);
  }
  png_init_io(png, f);
  png_read_info(png, info);
  const int depth = png_get_bit_depth(png, info);
  const int ctype = png_get_color_type(png, info);
  if (depth == 16) png_error(png, "16-bit PNG is not supported");
  if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  png_set_strip_alpha(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);
  const int out_ch = png_get_channels(png, info);
  if (out_ch != 1 && out_ch != 3) png_error(png, "unexpected channel count");
  const int w = int(png_get_image_width(png, info)), h = int(png_get_image_height(png, info));
  *img = Image(w, h, out_ch == 1 ? ColorFormat::Gray8 : ColorFormat::RGB8);
  std::vector<png_bytep> rows(static_cast<size_t>(h), nullptr);
  for (int y = 0; y < h; ++y) rows[size_t(y)] = img->row(y);
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);
  return std::move(*img);
}

}  // namespace

Image load_image(const std::string& path) {
  FilePtr f = open_file(path, "rb");
  unsigned char sig[8] = {0};
  const size_t n = std::fread(sig, 1, sizeof(sig), f.get());
  std::rewind(f.get());
  if (n >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF) return load_jpeg(f.get(), path);
  if (n == 8 && png_sig_cmp(sig, 0, 8) == 0) return load_png(f.get(), path);
  throw IoError("unsupported image format (expected JPEG or PNG): '" + path + "'");
}

void save_png(const std::string& path, const Image& img) {
  if (img.width <= 0 || img.height <= 0) throw IoError("save_png: empty image");
  FilePtr f = open_file(path, "wb");
  PngErr perr;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &perr, png_error_fn, png_warn_fn);
  if (!png) throw IoError("png_create_write_struct failed");
  png_infop info = png_create_info_struct(png);
  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    throw IoError("PNG encode failed for '" + path + "': " + perr.msg);
  }
  png_init_io(png, f.get());
  png_set_IHDR(png, info, png_uint_32(img.width), png_uint_32(img.height), 8,
               img.format == ColorFormat::Gray8 ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  for (int y = 0; y < img.height; ++y) png_write_row(png, img.row(y));
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  if (std::fflush(f.get()) != 0) throw IoError("write failed for '" + path + "'");
}

}  // namespace prmd
