#include <sstream>
#include <thread>
#include <vector>

#include "test_framework.h"
#include "tracker/tracker_state.h"

using namespace p2p;

TEST(create_user_rejects_duplicates) {
  TrackerState s;
  CHECK(s.create_user("alice", "pw").ok);
  CHECK(!s.create_user("alice", "other").ok);
}

TEST(login_checks_credentials) {
  TrackerState s;
  s.create_user("alice", "pw");
  CHECK(!s.login("nobody", "pw", "127.0.0.1", "1").ok);
  CHECK(!s.login("alice", "wrong", "127.0.0.1", "1").ok);
  CHECK(s.login("alice", "pw", "127.0.0.1", "1").ok);
}

TEST(group_creation_and_duplicate_ids) {
  TrackerState s;
  s.create_user("alice", "pw");
  CHECK(s.create_group("g1", "alice").ok);
  CHECK(!s.create_group("g1", "alice").ok);      // id taken
  CHECK(!s.create_group("g2", "ghost").ok);      // unknown user
}

TEST(join_requires_approval_by_the_owner) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.create_user("bob", "pw");
  s.create_group("g1", "alice");

  CHECK(s.join_group("g1", "bob").ok);
  // bob is only an applicant until approved, so group-member operations
  // must still refuse him.
  CHECK(!s.list_files("g1", "bob").ok);
  // and he cannot approve himself
  CHECK(!s.accept_request("g1", "bob", "bob").ok);
  CHECK(s.accept_request("g1", "bob", "alice").ok);
  CHECK(s.list_files("g1", "bob").ok);
}

TEST(reject_request_removes_without_admitting) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.create_user("bob", "pw");
  s.create_group("g1", "alice");
  s.join_group("g1", "bob");

  CHECK(!s.reject_request("g1", "bob", "bob").ok);   // not the owner
  CHECK(s.reject_request("g1", "bob", "alice").ok);
  CHECK(!s.reject_request("g1", "bob", "alice").ok); // nothing pending now
  CHECK(!s.list_files("g1", "bob").ok);              // still not a member
}

TEST(leave_group_hands_ownership_to_a_remaining_member) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.create_user("bob", "pw");
  s.create_group("g1", "alice");
  s.join_group("g1", "bob");
  s.accept_request("g1", "bob", "alice");

  CHECK(s.leave_group("g1", "alice").ok);
  // bob is the only member left, so he must now be able to run
  // owner-only operations.
  CHECK(s.list_requests("g1", "bob").ok);
}

TEST(upload_and_download_metadata_round_trip) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.login("alice", "pw", "127.0.0.1", "6000");
  s.create_group("g1", "alice");

  std::vector<std::string> hashes = {std::string(40, 'a'), std::string(40, 'b')};
  CHECK(s.upload_file("g1", "f.bin", "alice", 1000, std::string(40, 'c'), 2, hashes).ok);

  Result r = s.download_file("g1", "f.bin", "alice");
  CHECK(r.ok);
  CHECK(r.message.rfind("FILE ", 0) == 0);
  CHECK(r.message.find("SIZE 1000") != std::string::npos);
  CHECK(r.message.find(std::string(40, 'a')) != std::string::npos);
  // alice is logged in, so she should be listed as a reachable seeder
  CHECK(r.message.find("alice 127.0.0.1 6000") != std::string::npos);
}

TEST(disconnected_seeders_are_not_advertised) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.login("alice", "pw", "127.0.0.1", "6000");
  s.create_group("g1", "alice");
  s.upload_file("g1", "f.bin", "alice", 10, std::string(40, 'c'), 1,
                {std::string(40, 'a')});

  // Simulate the connection dropping. The seeder still holds the file,
  // but must not be handed out as reachable.
  s.handle_disconnect("alice");
  Result r = s.download_file("g1", "f.bin", "alice");
  CHECK(r.ok);
  CHECK(r.message.find("127.0.0.1 6000") == std::string::npos);
}

TEST(non_members_cannot_read_or_upload) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.create_user("mallory", "pw");
  s.create_group("g1", "alice");

  CHECK(!s.list_files("g1", "mallory").ok);
  CHECK(!s.download_file("g1", "f.bin", "mallory").ok);
  CHECK(!s.upload_file("g1", "f.bin", "mallory", 10, std::string(40, 'c'), 1,
                       {std::string(40, 'a')}).ok);
}

TEST(stop_share_removes_only_that_seeder) {
  TrackerState s;
  s.create_user("alice", "pw"); s.login("alice", "pw", "127.0.0.1", "1");
  s.create_user("bob", "pw");   s.login("bob", "pw", "127.0.0.1", "2");
  s.create_group("g1", "alice");
  s.join_group("g1", "bob");
  s.accept_request("g1", "bob", "alice");
  s.upload_file("g1", "f.bin", "alice", 10, std::string(40, 'c'), 1,
                {std::string(40, 'a')});
  s.file_downloaded("g1", "f.bin", "bob");

  CHECK(s.download_file("g1", "f.bin", "alice").message.find("bob") !=
        std::string::npos);
  CHECK(s.stop_share("g1", "f.bin", "bob").ok);
  Result r = s.download_file("g1", "f.bin", "alice");
  CHECK(r.message.find("bob") == std::string::npos);
  CHECK(r.message.find("alice") != std::string::npos);
}

// Not a proof of thread safety -- that is what the ThreadSanitizer run in
// tests/integration/concurrency_test.sh is for -- but it catches gross
// breakage such as a lost update or a self-deadlock in the locking
// discipline, and it runs in milliseconds on every build.
TEST(concurrent_writers_do_not_lose_updates) {
  TrackerState s;
  const int kThreads = 8;
  const int kPerThread = 50;

  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&s, t] {
      for (int i = 0; i < kPerThread; ++i) {
        s.create_user("u" + std::to_string(t) + "_" + std::to_string(i), "pw");
        s.create_group("g" + std::to_string(t) + "_" + std::to_string(i),
                       "u" + std::to_string(t) + "_" + std::to_string(i));
        s.list_groups();
      }
    });
  }
  for (auto &th : ts) th.join();

  // Every create_group above should have landed.
  Result r = s.list_groups();
  size_t count = 0;
  for (size_t i = 0; (i = r.message.find("\ng", i)) != std::string::npos; ++i) {
    ++count;
  }
  CHECK_EQ(count, size_t(kThreads * kPerThread));
}

// Files are identified by (group, filename). Keyed by filename alone,
// an upload to one group silently overwrote an unrelated file of the
// same name in another group.
TEST(same_filename_in_two_groups_stays_independent) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.login("alice", "pw", "127.0.0.1", "1");
  s.create_group("g1", "alice");
  s.create_group("g2", "alice");

  s.upload_file("g1", "data.bin", "alice", 100, std::string(40, 'a'), 1,
                {std::string(40, '1')});
  s.upload_file("g2", "data.bin", "alice", 999999, std::string(40, 'b'), 2,
                {std::string(40, '2'), std::string(40, '2')});

  Result r1 = s.download_file("g1", "data.bin", "alice");
  Result r2 = s.download_file("g2", "data.bin", "alice");
  CHECK(r1.message.find("SIZE 100 ") != std::string::npos);
  CHECK(r1.message.find(std::string(40, 'a')) != std::string::npos);
  CHECK(r2.message.find("SIZE 999999 ") != std::string::npos);
  CHECK(r2.message.find(std::string(40, 'b')) != std::string::npos);
}

// A downloader builds its output path as <destination>/<filename>, so a
// filename carrying path separators could escape the destination
// directory if a downloader copied it out of list_files.
TEST(upload_rejects_path_traversal_filenames) {
  TrackerState s;
  s.create_user("alice", "pw");
  s.create_group("g1", "alice");
  const std::string h(40, 'a');

  CHECK(!s.upload_file("g1", "../../.bashrc", "alice", 10, h, 1, {h}).ok);
  CHECK(!s.upload_file("g1", "sub/dir.bin", "alice", 10, h, 1, {h}).ok);
  CHECK(!s.upload_file("g1", "..", "alice", 10, h, 1, {h}).ok);
  CHECK(!s.upload_file("g1", ".", "alice", 10, h, 1, {h}).ok);
  // an ordinary name still works
  CHECK(s.upload_file("g1", "ok.bin", "alice", 10, h, 1, {h}).ok);
}
