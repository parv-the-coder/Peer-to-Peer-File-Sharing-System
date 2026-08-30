#include "config.h"

#include <fstream>
#include <sstream>

#include "message.h"

namespace p2p {

std::vector<Endpoint> load_endpoints(const std::string &path) {
  std::vector<Endpoint> out;
  std::ifstream in(path);
  if (!in) {
    return out;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> t = split_args(line);
    if (t.size() < 2) {
      continue; // blank or malformed line
    }
    int port = 0;
    if (!parse_int(t[1], port) || port <= 0 || port > 65535) {
      continue;
    }
    out.push_back({t[0], t[1]});
  }
  return out;
}

} // namespace p2p
