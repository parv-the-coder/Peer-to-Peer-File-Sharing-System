#pragma once

#include <string>
#include <vector>

namespace p2p {

// Splits a command string on whitespace into tokens. Replaces the
// strtok(buf, " ") pattern used throughout the original code: strtok's
// internal state is a single shared static pointer (not thread-local in
// general), so calling it concurrently from multiple connection-handler
// threads is a data race even though each thread tokenizes its own
// buffer. This is a plain, reentrant, allocation-only alternative.
std::vector<std::string> split_args(const std::string &s);

} // namespace p2p
