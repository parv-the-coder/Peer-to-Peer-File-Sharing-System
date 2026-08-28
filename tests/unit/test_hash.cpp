#include <sstream>

#include "common/hash.h"
#include "test_framework.h"

using namespace p2p;

// Known-answer tests against published digests, so a broken build or a
// mis-wired EVP call fails here rather than silently producing hashes
// that are self-consistent but wrong -- which would still pass an
// end-to-end transfer test, since both sides would agree.
TEST(sha1_known_answers) {
  CHECK_EQ(sha1_hex("", 0), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
  CHECK_EQ(sha1_hex("abc", 3), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
  const char *msg = "The quick brown fox jumps over the lazy dog";
  CHECK_EQ(sha1_hex(msg, 43), std::string("2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"));
}

TEST(sha256_known_answers) {
  CHECK_EQ(sha256_hex("", 0),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256_hex("abc", 3),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(sha1_hex_length_is_40) {
  CHECK_EQ(sha1_hex("x", 1).size(), size_t(40));
  CHECK_EQ(sha256_hex("x", 1).size(), size_t(64));
}

TEST(sha1_file_missing_returns_empty) {
  CHECK_EQ(sha1_file_hex("/nonexistent/path/does/not/exist"), std::string(""));
}

TEST(piece_hashes_missing_file_reports_zero_pieces) {
  long long n = -1;
  auto v = sha1_file_pieces("/nonexistent/path/does/not/exist", n);
  CHECK(v.empty());
  CHECK_EQ(n, 0LL);
}
