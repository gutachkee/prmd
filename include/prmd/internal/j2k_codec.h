#pragma once
// JPEG2000 backend (OpenJPEG).

#include <memory>
#include <string>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"

namespace prmd::internal {

EncodeReport encode_j2k(const Image& src,
                        const std::string& path,
                        const EncodeOptions& opts);
std::unique_ptr<CropDecoder> open_j2k(const std::string& path,
                                      const DecoderOptions& opts);

}  // namespace prmd::internal
