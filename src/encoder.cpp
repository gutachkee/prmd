#include "prmd/encoder.h"

#include <stdexcept>

#include "prmd/internal/j2k_codec.h"
#include "prmd/internal/prmd_codec.h"

namespace prmd {

const char* to_string(Transform t) {
  switch (t) {
    case Transform::Haar:
      return "haar";
    case Transform::Cdf53:
      return "cdf53";
    case Transform::Laplacian:
      return "laplacian";
    case Transform::Independent:
      return "naive";
  }
  return "?";
}

const char* to_string(Downsampler d) {
  switch (d) {
    case Downsampler::Box:
      return "box";
    case Downsampler::Gauss:
      return "gauss";
  }
  return "?";
}

const char* to_string(Entropy e) {
  switch (e) {
    case Entropy::Range:
      return "range";
    case Entropy::Zstd:
      return "zstd";
    case Entropy::Raw:
      return "raw";
  }
  return "?";
}

EncodeReport encode_file(const Image& src,
                         const std::string& out_path,
                         Codec codec,
                         const EncodeOptions& opts) {
  switch (codec) {
    case Codec::Prmd:
      return internal::encode_prmd(src, out_path, opts);
    case Codec::J2K:
      return internal::encode_j2k(src, out_path, opts);
  }
  throw std::invalid_argument("unknown codec");
}

}  // namespace prmd
