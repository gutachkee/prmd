#pragma once
// Image file I/O: JPEG (libjpeg-turbo) and PNG (libpng).

#include <string>

#include "prmd/image.h"

namespace prmd {

// Loads in the file's native format: grayscale JPEG/PNG → Gray8; color → RGB8.
// .jpg/.jpeg: libjpeg-turbo. .png: libpng (alpha dropped, palette expanded, 16-bit rejected).
// The format is detected from the file signature, not the extension.
// Throws prmd::IoError on failure.
Image load_image(const std::string& path);

// Writes a Gray8 or RGB8 PNG. Throws prmd::IoError on failure.
void save_png(const std::string& path, const Image& img);

}  // namespace prmd
