#include "prmd/image_io.h"

#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>

#include <jpeglib.h>
#include <png.h>

#include "prmd/errors.h"
#include "prmd/profile.h"

namespace prmd {

namespace {

struct FileCloser {
  void operator()(FILE* f) const {
    if (f) {
      std::fclose(f);
    }
  }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr open_file(const std::string& path, const char* mode) {
  FilePtr f(std::fopen(path.c_str(), mode));
  if (!f) {
    throw IoError("cannot open '" + path + "': " + std::strerror(errno));
  }
  return f;
}

ColorFormat format_for_channels(int channels) {
  return channels == 1 ? ColorFormat::Gray8 : ColorFormat::RGB8;
}

// ----------------------------------------------------------------------------
// JPEG

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

  // Everything touched after setjmp and read after longjmp lives outside this
  // frame's registers (the Image is heap-backed through unique_ptr).
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
    JSAMPROW row = img->row(int(cinfo.output_scanline));
    jpeg_read_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return std::move(*img);
}

// ----------------------------------------------------------------------------
// PNG

struct PngErr {
  char msg[256] = {0};
};

void png_error_fn(png_structp png, png_const_charp msg) {
  auto* e = static_cast<PngErr*>(png_get_error_ptr(png));
  if (e) {
    std::snprintf(e->msg, sizeof(e->msg), "%s", msg);
  }
  png_longjmp(png, 1);
}

void png_warn_fn(png_structp, png_const_charp) {}

// Asks libpng to deliver 8-bit gray or RGB rows whatever the stored format.
void configure_png_input(png_structp png, png_infop info) {
  const int depth = png_get_bit_depth(png, info);
  const int color_type = png_get_color_type(png, info);
  if (depth == 16) {
    png_error(png, "16-bit PNG is not supported");
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY && depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS)) {
    png_set_tRNS_to_alpha(png);
  }
  png_set_strip_alpha(png);
  png_set_interlace_handling(png);
  png_read_update_info(png, info);
}

Image load_png(FILE* f, const std::string& path) {
  PngErr perr;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &perr,
                                           png_error_fn, png_warn_fn);
  if (!png) {
    throw IoError("png_create_read_struct failed");
  }
  png_infop info = png_create_info_struct(png);

  // Heap objects created before setjmp: a longjmp out of libpng must not
  // skip the destructor of anything constructed after it.
  auto img = std::make_unique<Image>();
  auto rows = std::make_unique<std::vector<png_bytep>>();
  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    throw IoError("PNG decode failed for '" + path + "': " + perr.msg);
  }

  png_init_io(png, f);
  png_read_info(png, info);
  configure_png_input(png, info);

  const int out_channels = png_get_channels(png, info);
  if (out_channels != 1 && out_channels != 3) {
    png_error(png, "unexpected channel count");
  }
  const int w = int(png_get_image_width(png, info));
  const int h = int(png_get_image_height(png, info));
  *img = Image(w, h, format_for_channels(out_channels));

  rows->assign(static_cast<size_t>(h), nullptr);
  for (int y = 0; y < h; ++y) {
    (*rows)[size_t(y)] = img->row(y);
  }
  png_read_image(png, rows->data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);
  return std::move(*img);
}

bool is_jpeg_signature(const unsigned char* sig, size_t n) {
  return n >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF;
}

bool is_png_signature(const unsigned char* sig, size_t n) {
  return n == 8 && png_sig_cmp(sig, 0, 8) == 0;
}

}  // namespace

Image load_image(const std::string& path) {
  // Outside load_jpeg: its libjpeg error path longjmps, which would skip a
  // timer's destructor inside that frame.
  PRMD_TIMED("load_image");
  FilePtr f = open_file(path, "rb");
  unsigned char sig[8] = {0};
  const size_t n = std::fread(sig, 1, sizeof(sig), f.get());
  std::rewind(f.get());

  if (is_jpeg_signature(sig, n)) {
    return load_jpeg(f.get(), path);
  }
  if (is_png_signature(sig, n)) {
    return load_png(f.get(), path);
  }
  throw IoError("unsupported image format (expected JPEG or PNG): '" + path +
                "'");
}

void save_png(const std::string& path, const Image& img) {
  if (img.width <= 0 || img.height <= 0) {
    throw IoError("save_png: empty image");
  }
  FilePtr f = open_file(path, "wb");

  PngErr perr;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &perr,
                                            png_error_fn, png_warn_fn);
  if (!png) {
    throw IoError("png_create_write_struct failed");
  }
  png_infop info = png_create_info_struct(png);
  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    throw IoError("PNG encode failed for '" + path + "': " + perr.msg);
  }

  png_init_io(png, f.get());
  const int color_type = img.format == ColorFormat::Gray8 ? PNG_COLOR_TYPE_GRAY
                                                          : PNG_COLOR_TYPE_RGB;
  png_set_IHDR(png, info, png_uint_32(img.width), png_uint_32(img.height), 8,
               color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  for (int y = 0; y < img.height; ++y) {
    png_write_row(png, img.row(y));
  }
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);

  if (std::fflush(f.get()) != 0) {
    throw IoError("write failed for '" + path + "'");
  }
}

}  // namespace prmd
