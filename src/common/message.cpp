#include "message.h"

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

} // namespace p2p
