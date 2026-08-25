#pragma once

#include <string>

namespace p2p {

// Number of random bytes in a per-user password salt.
constexpr size_t kSaltBytes = 16;
// Number of random bytes in a session token (256 bits).
constexpr size_t kTokenBytes = 32;

// Cryptographically random hex string of `bytes` bytes, from the OS CSPRNG
// via OpenSSL's RAND_bytes. Returns an empty string if the CSPRNG fails,
// which callers must treat as an error rather than proceeding with a
// predictable value.
std::string random_hex(size_t bytes);

// A stored credential. The plaintext password is never retained.
struct PasswordHash {
  std::string salt; // hex
  std::string hash; // hex, sha256(salt || password)
};

// Hashes `password` with a freshly generated random salt.
PasswordHash hash_password(const std::string &password);

// Recomputes the hash for `password` under the stored salt and compares
// it against the stored hash.
bool verify_password(const std::string &password, const PasswordHash &stored);

} // namespace p2p
