#include "auth.h"

#include <openssl/rand.h>

#include <vector>

#include "hash.h"

namespace p2p {
namespace {

std::string to_hex(const unsigned char *data, size_t len) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0f]);
  }
  return out;
}

} // namespace

std::string random_hex(size_t bytes) {
  std::vector<unsigned char> buf(bytes);
  // RAND_bytes returns 1 on success. Anything else means we do not have
  // real entropy; returning "" lets the caller fail loudly rather than
  // mint a guessable salt or token.
  if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
    return "";
  }
  return to_hex(buf.data(), bytes);
}

PasswordHash hash_password(const std::string &password) {
  PasswordHash ph;
  ph.salt = random_hex(kSaltBytes);
  if (ph.salt.empty()) {
    return ph; // caller checks for an empty salt
  }
  const std::string salted = ph.salt + password;
  ph.hash = sha256_hex(salted.data(), salted.size());
  return ph;
}

bool verify_password(const std::string &password, const PasswordHash &stored) {
  if (stored.salt.empty() || stored.hash.empty()) {
    return false;
  }
  const std::string salted = stored.salt + password;
  const std::string computed = sha256_hex(salted.data(), salted.size());
  if (computed.size() != stored.hash.size()) {
    return false;
  }
  // Constant-time comparison: a plain == returns as soon as it finds a
  // differing byte, which leaks how much of the hash a guess got right.
  unsigned char diff = 0;
  for (size_t i = 0; i < computed.size(); ++i) {
    diff |= static_cast<unsigned char>(computed[i] ^ stored.hash[i]);
  }
  return diff == 0;
}

} // namespace p2p
