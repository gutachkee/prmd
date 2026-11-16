#pragma once
// .prmd encoder/decoder entry points (used by encode_file / open_decoder).

#include <cstdint>
#include <memory>
#include <string>

#include "prmd/crop_decoder.h"
#include "prmd/encoder.h"

namespace prmd::internal {

class BlockCoder;

// `coder` replaces the entropy coder chosen by opts.entropy (tests: fault
// injection); null = make_block_coder(opts.entropy, opts.entropy_level).
EncodeReport encode_prmd(const Image& src,
                         const std::string& path,
                         const EncodeOptions& opts,
                         const BlockCoder* coder = nullptr);
std::unique_ptr<CropDecoder> open_prmd(const std::string& path,
                                       const DecoderOptions& opts);

// Throws std::invalid_argument describing the first invalid option.
void validate_prmd_options(const EncodeOptions& opts);

// Work units (parallel.h, auto_threads) of a .prmd encode of a
// width × height image: level-0 samples × channels, weighted by the
// relative cost of the transform and the entropy coder. Input to
// auto_threads for EncodeOptions::threads = 0.
uint64_t prmd_work_units(int width, int height, const EncodeOptions& opts);

}  // namespace prmd::internal
