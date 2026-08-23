#pragma once

#include <cstddef>
#include <string>

namespace p2p {

// Default cap on a single framed message. Bounds the buffer we're willing
// to allocate for a peer-supplied length prefix, so a malicious/buggy peer
// can't make us try to allocate gigabytes before we've even validated
// anything about the message.
constexpr size_t kMaxFrameSize = 16ull * 1024 * 1024; // 16 MB

// Reads exactly n bytes into buf, looping over partial reads. Returns
// false on EOF or error before n bytes were read.
bool recv_all(int fd, void *buf, size_t n);

// Writes exactly n bytes from buf, looping over partial writes. Returns
// false on error before n bytes were written.
bool send_all(int fd, const void *buf, size_t n);

// Sends payload as [4-byte big-endian length][payload bytes]. This is the
// framing envelope used for every tracker<->client and peer<->peer
// message, so that a message spanning multiple TCP segments (e.g. an
// upload_file command with hundreds of piece hashes) is never mistaken
// for a complete read after a single recv().
bool send_framed(int fd, const std::string &payload);

// Reads one framed message: the 4-byte length prefix, then that many
// payload bytes. Returns false on EOF, error, or a length prefix that
// exceeds max_len (treated as a protocol violation, not a valid message).
bool recv_framed(int fd, std::string &out, size_t max_len = kMaxFrameSize);

} // namespace p2p
