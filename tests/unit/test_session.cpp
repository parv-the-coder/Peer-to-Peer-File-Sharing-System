#include <set>
#include <sstream>

#include "test_framework.h"
#include "tracker/session_manager.h"

using namespace p2p;

TEST(session_create_and_lookup) {
  SessionManager sm;
  std::string tok = sm.create("alice", "127.0.0.1", "6000");
  CHECK(!tok.empty());

  SessionInfo info;
  CHECK(sm.lookup(tok, info));
  CHECK_EQ(info.username, std::string("alice"));
  CHECK_EQ(info.ip, std::string("127.0.0.1"));
  CHECK_EQ(info.port, std::string("6000"));
}

TEST(unknown_and_empty_tokens_are_rejected) {
  SessionManager sm;
  sm.create("alice", "127.0.0.1", "6000");
  SessionInfo info;
  CHECK(!sm.lookup("", info));
  CHECK(!sm.lookup(std::string(64, '0'), info));
  CHECK(!sm.lookup("not-a-token", info));
}

TEST(tokens_are_unique_per_session) {
  SessionManager sm;
  std::set<std::string> toks;
  for (int i = 0; i < 20; ++i) toks.insert(sm.create("alice", "127.0.0.1", "6000"));
  // Same user logging in repeatedly must not be handed the same token.
  CHECK_EQ(toks.size(), size_t(20));
}

// The impersonation fix depends on this: a token must stop working the
// moment its session ends, or logout is cosmetic.
TEST(token_is_dead_after_destroy) {
  SessionManager sm;
  std::string tok = sm.create("bob", "127.0.0.1", "6001");
  std::string user;
  CHECK(sm.username_for(tok, user));
  CHECK_EQ(user, std::string("bob"));

  CHECK_EQ(sm.destroy(tok), std::string("bob"));
  CHECK(!sm.username_for(tok, user));
  // Destroying an already-dead token reports no user rather than lying.
  CHECK_EQ(sm.destroy(tok), std::string(""));
}

TEST(destroy_all_for_revokes_every_session_of_one_user) {
  SessionManager sm;
  std::string a1 = sm.create("alice", "127.0.0.1", "1");
  std::string a2 = sm.create("alice", "127.0.0.1", "2");
  std::string b1 = sm.create("bob", "127.0.0.1", "3");

  sm.destroy_all_for("alice");
  std::string user;
  CHECK(!sm.username_for(a1, user));
  CHECK(!sm.username_for(a2, user));
  CHECK(sm.username_for(b1, user)); // bob is untouched
  CHECK_EQ(user, std::string("bob"));
}
