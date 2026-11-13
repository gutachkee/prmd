#pragma once
// Error types. Bad level or rect: std::out_of_range. Bad EncodeOptions:
// std::invalid_argument.

#include <stdexcept>

namespace prmd {

// open/read/write failures
struct IoError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// bad magic/version/CRC/payload
struct FormatError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace prmd
