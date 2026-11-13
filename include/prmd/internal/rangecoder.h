#pragma once
// Adaptive binary range coder (LZMA-style carry-less encoder with cache byte).
// 12-bit probabilities, adaptation shift 5. No tables, SIMD or FPU.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace prmd::internal {

using Prob = uint16_t;  // P(bit == 0) in 1/4096 units
constexpr int kProbBits = 12;
constexpr int kAdaptShift = 5;
constexpr Prob kProbInit = Prob(1u << (kProbBits - 1));  // 2048 = 50%

// Renormalization threshold shared by encoder and decoder.
constexpr uint32_t kRangeTop = 1u << 24;

// Probability adaptation after coding a 0 / a 1.
inline void adapt_after_zero(Prob& p) {
  p = Prob(p + (((1u << kProbBits) - p) >> kAdaptShift));
}

inline void adapt_after_one(Prob& p) {
  p = Prob(p - (p >> kAdaptShift));
}

class RangeEncoder {
 public:
  explicit RangeEncoder(std::vector<uint8_t>& out) : out_(out) {}

  void encode_bit(Prob& p, int bit) {
    const uint32_t bound = (range_ >> kProbBits) * p;
    if (bit == 0) {
      range_ = bound;
      adapt_after_zero(p);
    } else {
      low_ += bound;
      range_ -= bound;
      adapt_after_one(p);
    }
    normalize();
  }

  // Equiprobable bits, most significant first. nbits in [0, 24].
  void encode_direct(uint32_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; --i) {
      range_ >>= 1;
      if ((value >> i) & 1u) {
        low_ += range_;
      }
      normalize();
    }
  }

  // Flushes the remaining state. Must be called exactly once, after the last
  // symbol.
  void finish() {
    for (int i = 0; i < 5; ++i) {
      shift_low();
    }
  }

 private:
  void normalize() {
    while (range_ < kRangeTop) {
      range_ <<= 8;
      shift_low();
    }
  }

  void shift_low() {
    if (uint32_t(low_) < 0xFF000000u || (low_ >> 32) != 0) {
      const uint8_t carry = uint8_t(low_ >> 32);
      uint8_t temp = cache_;
      do {
        out_.push_back(uint8_t(temp + carry));
        temp = 0xFF;
      } while (--cache_size_ != 0);
      cache_ = uint8_t(low_ >> 24);
    }
    ++cache_size_;
    low_ = (low_ & 0x00FFFFFFu) << 8;
  }

  std::vector<uint8_t>& out_;
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
};

class RangeDecoder {
 public:
  // Reading past the end of `data` yields zero bytes (no UB); the caller
  // detects truncation with overrun().
  RangeDecoder(const uint8_t* data, size_t size) : p_(data), end_(data + size) {
    for (int i = 0; i < 5; ++i) {
      code_ = (code_ << 8) | next_byte();
    }
  }

  int decode_bit(Prob& p) {
    const uint32_t bound = (range_ >> kProbBits) * p;
    int bit;
    if (code_ < bound) {
      range_ = bound;
      adapt_after_zero(p);
      bit = 0;
    } else {
      code_ -= bound;
      range_ -= bound;
      adapt_after_one(p);
      bit = 1;
    }
    normalize();
    return bit;
  }

  uint32_t decode_direct(int nbits) {
    uint32_t v = 0;
    for (int i = 0; i < nbits; ++i) {
      range_ >>= 1;
      uint32_t bit = 0;
      if (code_ >= range_) {
        code_ -= range_;
        bit = 1;
      }
      v = (v << 1) | bit;
      normalize();
    }
    return v;
  }

  // Number of bytes requested beyond the end of the input. The encoder's 5-byte
  // flush means a well-formed stream never overruns.
  size_t overrun() const { return overrun_; }

 private:
  void normalize() {
    while (range_ < kRangeTop) {
      range_ <<= 8;
      code_ = (code_ << 8) | next_byte();
    }
  }

  uint32_t next_byte() {
    if (p_ < end_) {
      return *p_++;
    }
    ++overrun_;
    return 0;
  }

  const uint8_t* p_;
  const uint8_t* end_;
  uint32_t code_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  size_t overrun_ = 0;
};

}  // namespace prmd::internal
