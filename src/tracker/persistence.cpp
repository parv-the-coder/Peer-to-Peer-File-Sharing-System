#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "common/message.h"
#include "tracker/tracker_state.h"

// Snapshot / replication record format: one record per line, whitespace
// separated.
//
//   V 1
//   USER  <name> <salt> <hash>
//   GROUP <gid> <master> <nparts> <p...> <napps> <a...>
//   FILE  <groupid>/<fname> <size> <fullhash> <npieces> <h...> <npeers> <peer...>
//   GFILE <gid> <count> <fname...>
//
// FILE records store the map key verbatim, which is "<groupid>/<fname>"
// -- files are identified per group, not by filename alone.
//
// A whitespace-separated format is safe here rather than merely
// convenient: every one of these fields arrives over a protocol that is
// itself split on whitespace, so none of them can contain whitespace to
// begin with. That removes the quoting and escaping problem entirely and
// avoids pulling in a JSON dependency for a fixed, flat schema.
//
// The same format serves the on-disk snapshot and tracker-to-tracker
// replication, so there is one writer and one parser rather than two of
// each that can drift apart.

namespace p2p {

std::string TrackerState::serialize_locked() const {
  std::ostringstream out;
  out << "V 1\n";
  for (const auto &entry : peers_) {
    const Peer &p = entry.second;
    // Skip accounts whose credentials failed to generate; reloading one
    // would produce an account nobody can ever log into.
    if (p.password.salt.empty() || p.password.hash.empty()) {
      continue;
    }
    out << "USER " << entry.first << " " << p.password.salt << " "
        << p.password.hash << "\n";
  }
  for (const auto &entry : groups_) {
    const Group &g = entry.second;
    out << "GROUP " << entry.first << " "
        << (g.groupmaster.empty() ? "-" : g.groupmaster) << " "
        << g.participants.size();
    for (const auto &p : g.participants) out << " " << p;
    out << " " << g.applicants.size();
    for (const auto &a : g.applicants) out << " " << a;
    out << "\n";
  }
  for (const auto &entry : files_) {
    const FileMeta &f = entry.second;
    out << "FILE " << entry.first << " " << f.size << " " << f.fullhash << " "
        << f.piece_hashes.size();
    for (const auto &h : f.piece_hashes) out << " " << h;
    out << " " << f.peers.size();
    for (const auto &p : f.peers) out << " " << p;
    out << "\n";
  }
  for (const auto &entry : group_files_) {
    out << "GFILE " << entry.first << " " << entry.second.size();
    for (const auto &f : entry.second) out << " " << f;
    out << "\n";
  }
  return out.str();
}

std::string TrackerState::serialize() const {
  // Held for the whole walk: serialising without the lock could capture a
  // torn view, with a group written before an edit and a file after it.
  std::shared_lock lock(mtx_);
  return serialize_locked();
}

unsigned long long TrackerState::version() const {
  std::shared_lock lock(mtx_);
  return version_;
}

namespace {

// Reads a count followed by that many tokens. Returns false if the count
// is missing, negative, or the line is shorter than it claims -- a
// truncated or hand-edited record must be rejected, not read past.
bool take_list(const std::vector<std::string> &t, size_t &i, long long &count,
               std::vector<std::string> &out) {
  out.clear();
  if (i >= t.size() || !parse_ll(t[i], count) || count < 0) {
    return false;
  }
  ++i;
  if (count > (long long)(t.size() - i)) {
    return false;
  }
  for (long long k = 0; k < count; ++k) {
    out.push_back(t[i++]);
  }
  return true;
}

} // namespace

// replace=true  : this is a snapshot load, take it as the whole truth.
// replace=false : this is a merge from the other tracker, union it in.
bool TrackerState::apply_records_locked(const std::string &data, bool replace,
                                       bool *changed) {
  if (changed) *changed = false;
  auto touched = [&] { if (changed) *changed = true; };

  if (replace) {
    peers_.clear();
    groups_.clear();
    files_.clear();
    group_files_.clear();
  }

  std::istringstream in(data);
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> t = split_args(line);
    if (t.empty() || t[0] == "V") {
      continue;
    }

    if (t[0] == "USER") {
      if (t.size() != 4) return false;
      // On merge, an account already here wins: overwriting would swap in
      // a different salt/hash pair for the same person and could
      // invalidate a password they set on this tracker.
      if (!replace && peers_.find(t[1]) != peers_.end()) continue;
      touched();
      Peer p;
      p.peername = t[1];
      p.password.salt = t[2];
      p.password.hash = t[3];
      p.connected = false; // never restored: it describes a live socket
      peers_[t[1]] = std::move(p);
      continue;
    }

    if (t[0] == "GROUP") {
      if (t.size() < 4) return false;
      size_t i = 3;
      long long n = 0;
      std::vector<std::string> parts, apps;
      if (!take_list(t, i, n, parts)) return false;
      if (!take_list(t, i, n, apps)) return false;

      auto it = groups_.find(t[1]);
      if (it == groups_.end()) {
        touched();
        Group g;
        g.gid = t[1];
        g.groupmaster = (t[2] == "-") ? "" : t[2];
        g.participants.insert(parts.begin(), parts.end());
        g.applicants.insert(apps.begin(), apps.end());
        groups_[t[1]] = std::move(g);
      } else {
        // Union membership. An owner already set here is kept, so a
        // reconnecting tracker cannot reassign a group's owner.
        size_t before = it->second.participants.size() + it->second.applicants.size();
        it->second.participants.insert(parts.begin(), parts.end());
        it->second.applicants.insert(apps.begin(), apps.end());
        if (before != it->second.participants.size() + it->second.applicants.size()) {
          touched();
        }
        if (it->second.groupmaster.empty() && t[2] != "-") {
          it->second.groupmaster = t[2];
          touched();
        }
      }
      continue;
    }

    if (t[0] == "FILE") {
      if (t.size() < 5) return false;
      FileMeta f;
      if (!parse_ll(t[2], f.size) || f.size < 0) return false;
      f.fullhash = t[3];
      size_t i = 4;
      long long n = 0;
      std::vector<std::string> hashes, seeders;
      if (!take_list(t, i, n, hashes)) return false;
      if (!take_list(t, i, n, seeders)) return false;
      f.piece_hashes = hashes;
      f.num_pieces = (int)hashes.size();
      f.peers.insert(seeders.begin(), seeders.end());

      auto it = files_.find(t[1]);
      if (it == files_.end()) {
        touched();
        files_[t[1]] = std::move(f);
      } else {
        // Same file known to both: keep our metadata, union the seeders.
        size_t before = it->second.peers.size();
        it->second.peers.insert(seeders.begin(), seeders.end());
        if (before != it->second.peers.size()) touched();
      }
      continue;
    }

    if (t[0] == "GFILE") {
      if (t.size() < 3) return false;
      size_t i = 2;
      long long n = 0;
      std::vector<std::string> items;
      if (!take_list(t, i, n, items)) return false;
      {
        auto &set = group_files_[t[1]];
        size_t before = set.size();
        set.insert(items.begin(), items.end());
        if (before != set.size()) touched();
      }
      continue;
    }

    return false; // unknown record type
  }
  return true;
}

bool TrackerState::merge_from(const std::string &data) {
  std::unique_lock lock(mtx_);
  bool changed = false;
  bool ok = apply_records_locked(data, /*replace=*/false, &changed);
  // Only a merge that actually altered something counts as a new
  // version. Bumping unconditionally makes the two trackers ping-pong
  // forever: each push makes the receiver "change", so it pushes back,
  // which makes the sender change, and neither ever goes quiet.
  if (ok && changed) {
    ++version_;
  }
  return ok;
}

bool TrackerState::save_snapshot(const std::string &path) const {
  std::string data = serialize(); // takes the shared lock itself

  // Write to a temp file and rename it into place: rename is atomic
  // within a filesystem, so a crash mid-write leaves the previous good
  // snapshot intact rather than a truncated one.
  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp, std::ios::trunc);
  if (!out) {
    return false;
  }
  out << data;
  out.flush();
  if (!out) {
    out.close();
    std::remove(tmp.c_str());
    return false;
  }
  out.close();
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool TrackerState::load_snapshot(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return false; // no snapshot yet is not an error for the caller
  }
  std::ostringstream buf;
  buf << in.rdbuf();

  std::unique_lock lock(mtx_);
  return apply_records_locked(buf.str(), /*replace=*/true, nullptr);
}

} // namespace p2p
