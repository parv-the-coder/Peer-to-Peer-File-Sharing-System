#pragma once

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace p2p {

// Outcome of a tracker command. `message` is the reply to send back to
// the client either way; ok=false simply means it describes a rejection.
struct Result {
  bool ok = false;
  std::string message;
};

// A registered peer and its last known listening address.
struct Peer {
  std::string hostip;
  std::string hostport;
  std::string peername;
  std::string passcode;
  bool connected = false;

  void login(const std::string &ip, const std::string &port);
  void logout();
};

// A group: an owner, its members, and pending join requests.
struct Group {
  std::string gid;
  std::string groupmaster;
  std::unordered_set<std::string> participants;
  std::unordered_set<std::string> applicants;

  bool isapplicant(const std::string &s) const;
  bool partofgroup(const std::string &s) const;
  void acceptreq(const std::string &s);
  // Removes s from the group. If s owned it, ownership passes to an
  // arbitrary remaining member, or is cleared when the group empties.
  void deluser(const std::string &s);
};

// Metadata for one shared file, plus the set of peers seeding it.
struct FileMeta {
  long long size = 0;
  std::string fullhash;
  int num_pieces = 0;
  std::vector<std::string> piece_hashes;
  std::unordered_set<std::string> peers;
};

// All mutable tracker state, behind one lock.
//
// Locking strategy: a single shared_mutex guards all four maps together.
// Per-map locks were rejected because most commands span several maps at
// once -- upload_file touches groups, files and group_files; download_file
// reads files and cross-references each seeder's connected flag in peers
// -- so per-map locking would need a lock-ordering discipline to stay
// deadlock-free while buying very little real concurrency at the scale
// this tracker runs at (tens of connections, not thousands).
//
// Every public method acquires the lock itself: shared for read-only
// queries, exclusive for anything that mutates. Private helpers suffixed
// _locked assume the lock is already held and must only ever be called
// from within a public method -- std::shared_mutex is not recursive, so
// re-entering through a public method would self-deadlock.
class TrackerState {
public:
  // Account and session lifecycle.
  Result create_user(const std::string &username, const std::string &passcode);
  Result login(const std::string &username, const std::string &passcode,
               const std::string &ip, const std::string &port);
  Result logout(const std::string &username);

  // Group membership.
  Result create_group(const std::string &gid, const std::string &owner);
  Result join_group(const std::string &gid, const std::string &username);
  Result leave_group(const std::string &gid, const std::string &username);
  Result list_requests(const std::string &gid, const std::string &owner) const;
  Result accept_request(const std::string &gid, const std::string &applicant,
                        const std::string &owner);
  Result list_groups() const;

  // Files.
  Result upload_file(const std::string &gid, const std::string &fname,
                     const std::string &uname, long long size,
                     const std::string &fullhash, int num_pieces,
                     const std::vector<std::string> &piece_hashes);
  Result list_files(const std::string &gid, const std::string &uname) const;
  Result download_file(const std::string &gid, const std::string &fname,
                       const std::string &uname) const;
  Result file_downloaded(const std::string &gid, const std::string &fname,
                         const std::string &peername);
  Result stop_share(const std::string &gid, const std::string &fname,
                    const std::string &peername);

  // Called when a connection drops, gracefully or not.
  void handle_disconnect(const std::string &username);

private:
  bool user_exists_locked(const std::string &name) const;
  bool group_exists_locked(const std::string &gid) const;
  // Non-inserting lookup. Using group_files_[gid] here would mutate the
  // map, which would make the read-only query paths unsafe under a
  // shared lock.
  bool group_has_file_locked(const std::string &gid,
                             const std::string &fname) const;

  mutable std::shared_mutex mtx_;
  std::unordered_map<std::string, Peer> peers_;
  std::unordered_map<std::string, Group> groups_;
  std::unordered_map<std::string, std::unordered_set<std::string>> group_files_;
  std::unordered_map<std::string, FileMeta> files_;
};

} // namespace p2p
