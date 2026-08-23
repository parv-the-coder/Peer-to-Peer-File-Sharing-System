#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace p2p {

// Size of one file piece. Files are split into pieces of this size; each
// piece is hashed independently so a download can verify and retry
// per-piece rather than only detecting corruption at the end.
constexpr size_t kPieceSize = 512 * 1024; // 512 KB

// SHA-1 of an in-memory buffer, lowercase hex. Used for piece integrity.
std::string sha1_hex(const void *data, size_t len);

// SHA-1 of an entire file, streamed, lowercase hex. Empty string if the
// file can't be opened.
std::string sha1_file_hex(const std::string &path);

// Per-piece SHA-1 hashes of a file, in order. Sets num_pieces to the
// piece count. Empty result if the file can't be opened.
std::vector<std::string> sha1_file_pieces(const std::string &path,
                                          long long &num_pieces);

// SHA-256 of an in-memory buffer, lowercase hex.
std::string sha256_hex(const void *data, size_t len);

} // namespace p2p
