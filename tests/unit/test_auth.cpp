#include <set>
#include <sstream>

#include "common/auth.h"
#include "test_framework.h"

using namespace p2p;

TEST(random_hex_length_and_variation) {
  std::string a = random_hex(16);
  CHECK_EQ(a.size(), size_t(32)); // hex is 2 chars per byte
  // Distinct draws: a CSPRNG returning a constant would break every
  // salt and token in the system at once.
  std::set<std::string> seen;
  for (int i = 0; i < 20; ++i) seen.insert(random_hex(16));
  CHECK_EQ(seen.size(), size_t(20));
}

TEST(password_round_trip) {
  PasswordHash ph = hash_password("correct horse battery staple");
  CHECK(!ph.salt.empty());
  CHECK(!ph.hash.empty());
  CHECK(verify_password("correct horse battery staple", ph));
  CHECK(!verify_password("wrong password", ph));
  CHECK(!verify_password("", ph));
}

TEST(password_is_not_stored_in_plaintext) {
  const std::string pw = "hunter2";
  PasswordHash ph = hash_password(pw);
  CHECK(ph.hash.find(pw) == std::string::npos);
  CHECK(ph.salt.find(pw) == std::string::npos);
}

TEST(same_password_different_salts_gives_different_hashes) {
  PasswordHash a = hash_password("same");
  PasswordHash b = hash_password("same");
  CHECK(a.salt != b.salt);
  // This is the point of per-user salts: identical passwords must not
  // produce identical stored digests, or a leaked table reveals which
  // accounts share a password and makes one cracked hash reusable.
  CHECK(a.hash != b.hash);
  CHECK(verify_password("same", a));
  CHECK(verify_password("same", b));
}

TEST(verify_rejects_empty_or_corrupt_record) {
  PasswordHash empty;
  CHECK(!verify_password("anything", empty));

  PasswordHash ph = hash_password("pw");
  PasswordHash no_hash = ph; no_hash.hash.clear();
  CHECK(!verify_password("pw", no_hash));
  PasswordHash truncated = ph; truncated.hash = ph.hash.substr(0, 10);
  CHECK(!verify_password("pw", truncated));
}

TEST(verify_is_case_sensitive) {
  PasswordHash ph = hash_password("Secret");
  CHECK(!verify_password("secret", ph));
  CHECK(verify_password("Secret", ph));
}
