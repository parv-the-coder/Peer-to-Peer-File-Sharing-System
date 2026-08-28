#include "message.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace p2p {

std::vector<std::string> split_args(const std::string &s) {
  std::vector<std::string> tokens;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) {
    tokens.push_back(tok);
  }
  return tokens;
}

bool parse_ll(const std::string &s, long long &out) {
  if (s.empty()) {
    return false;
  }
  // strtoll silently skips leading whitespace, which contradicts the
  // "complete integer or nothing" contract these are documented with.
  if (std::isspace(static_cast<unsigned char>(s[0]))) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno == ERANGE || end != s.c_str() + s.size()) {
    return false;
  }
  out = v;
  return true;
}

bool parse_int(const std::string &s, int &out) {
  long long v = 0;
  if (!parse_ll(s, v)) {
    return false;
  }
  if (v < std::numeric_limits<int>::min() ||
      v > std::numeric_limits<int>::max()) {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

} // namespace p2p
