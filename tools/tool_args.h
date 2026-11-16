#pragma once
// Strict numeric arguments for prmdbench and prmdpipeline: the whole string
// must be a decimal integer in range (no std::stoi, which accepts "12abc"
// and throws on junk).

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace prmd::tools {

// Parses `s` as a base-10 integer in [lo, hi]; false if it is empty, has
// any other character, or is out of range.
inline bool parse_int(const std::string& s, long long lo, long long hi,
                      long long* out) {
  if (s.empty()) {
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
