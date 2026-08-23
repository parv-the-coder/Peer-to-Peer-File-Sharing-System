#include "hash.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

namespace {

std::string to_hex(const unsigned char *digest, unsigned int len) {
  static const char *hex_digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (unsigned int i = 0; i < len; ++i) {
    out.push_back(hex_digits[digest[i] >> 4]);
    out.push_back(hex_digits[digest[i] & 0x0f]);
  }
  return out;
}

std::string digest_hex(const EVP_MD *md, const void *data, size_t len) {
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    return "";
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  std::string result;
  if (EVP_DigestInit_ex(ctx, md, nullptr) == 1 &&
      EVP_DigestUpdate(ctx, data, len) == 1 &&
      EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1) {
    result = to_hex(digest, digest_len);
  }
  EVP_MD_CTX_free(ctx);
  return result;
}

} // namespace

namespace p2p {

std::string sha1_hex(const void *data, size_t len) {
  return digest_hex(EVP_sha1(), data, len);
}

std::string sha256_hex(const void *data, size_t len) {
  return digest_hex(EVP_sha256(), data, len);
}

std::string sha1_file_hex(const std::string &path) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return "";
  }
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    close(fd);
    return "";
  }
  std::string result;
  if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1) {
    std::vector<char> buf(kPieceSize);
    ssize_t n;
    bool ok = true;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
      if (EVP_DigestUpdate(ctx, buf.data(), n) != 1) {
        ok = false;
        break;
      }
    }
    if (ok && n >= 0) {
      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int digest_len = 0;
      if (EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1) {
        result = to_hex(digest, digest_len);
      }
    }
  }
  EVP_MD_CTX_free(ctx);
  close(fd);
  return result;
}

std::vector<std::string> sha1_file_pieces(const std::string &path,
                                          long long &num_pieces) {
  std::vector<std::string> hashes;
  num_pieces = 0;

  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return hashes;
  }

  off_t filesize = lseek(fd, 0, SEEK_END);
  if (filesize < 0 || lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    return hashes;
  }
  num_pieces = (filesize + static_cast<off_t>(kPieceSize) - 1) / kPieceSize;

  std::vector<char> buf(kPieceSize);
  for (long long i = 0; i < num_pieces; ++i) {
    ssize_t n = read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    hashes.push_back(sha1_hex(buf.data(), static_cast<size_t>(n)));
  }
  close(fd);
  return hashes;
}

} // namespace p2p
