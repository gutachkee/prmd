#pragma once
// Strict numeric arguments for prmd, prmdbench and prmdpipeline: the whole
// string must be a decimal integer in range (no std::stoi, which accepts
// "12abc" and throws on junk; and no strtoll leniency: leading whitespace
// and '+' are refused too).

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace prmd::tools {

// Whether `s` is one or more decimal digits, after one optional '-' when
// `negative` allows it: no whitespace, no '+', nothing after the digits.
inline bool is_decimal_integer(const std::string& s, bool negative) {
  size_t i = 0;
  if (negative && !s.empty() && s[0] == '-') {
    i = 1;
  }
  if (i == s.size()) {
    return false;
  }
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

// Parses `s` as a base-10 integer in [lo, hi]; false if it is not a
// decimal integer (is_decimal_integer, with a '-' only when lo < 0) or is
// out of range.
inline bool parse_int(const std::string& s, long long lo, long long hi,
                      long long* out) {
  if (!is_decimal_integer(s, lo < 0)) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != 0 || errno == ERANGE || v < lo || v > hi) {
    return false;
  }
  *out = v;
  return true;
}

// Comma-separated integers in [lo, hi] ("1,2,4,8,0"); false on an empty
// list, an empty item ("1,,2", "1,") or a bad item.
inline bool parse_int_list(const std::string& s, long long lo, long long hi,
                           std::vector<int>* out) {
  std::vector<int> v;
  std::string item;
  std::stringstream ss(s);
  while (std::getline(ss, item, ',')) {
    long long x = 0;
    if (!parse_int(item, lo, hi, &x)) {
      return false;
    }
    v.push_back(int(x));
  }
  if (v.empty() || (!s.empty() && s.back() == ',')) {
    return false;
  }
  *out = std::move(v);
  return true;
}

}  // namespace prmd::tools
