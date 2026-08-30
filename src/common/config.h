#pragma once

#include <string>
#include <vector>

namespace p2p {

struct Endpoint {
  std::string ip;
  std::string port;
};

// Parses a tracker info file: one "<ip> <port>" per line, one line per
// tracker. The assignment brief runs two trackers, selected by the
// tracker number passed on the command line.
//
// Returns an empty vector if the file cannot be read or contains no
// usable entry.
std::vector<Endpoint> load_endpoints(const std::string &path);

} // namespace p2p
