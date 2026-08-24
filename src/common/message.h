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

// Strict integer parsing for values taken off the wire. Returns false on
// anything that is not a complete, in-range integer -- including empty
// strings, trailing garbage ("12abc") and overflow.
//
// These exist because std::stoi throws on malformed input, and an
// uncaught throw inside a connection thread calls std::terminate and
// takes down the whole process. Any client could crash the tracker with
// one malformed command.
bool parse_int(const std::string &s, int &out);
bool parse_ll(const std::string &s, long long &out);

} // namespace p2p
