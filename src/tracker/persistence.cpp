#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

#include "common/message.h"
#include "tracker/tracker_state.h"

// Snapshot format: one record per line, whitespace separated.
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
// Writes go to a temporary file which is then rename()d over the target.
// rename is atomic within a filesystem, so a crash mid-write leaves the
// previous good snapshot intact rather than a truncated one.

namespace p2p {

bool TrackerState::save_snapshot(const std::string &path) const {
  // Held for the whole walk: serialising without the lock could capture a
  // torn view, with a group written before an edit and a file after it.
  std::shared_lock lock(mtx_);

  const std::string tmp = path + ".tmp";
  std::ofstream out(tmp, std::ios::trunc);
  if (!out) {
    return false;
  }

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

namespace {

// Reads a count followed by that many tokens. Returns false if the count
// is missing, negative, or the line is shorter than it claims -- a
// truncated or hand-edited snapshot must be rejected, not read past.
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

bool TrackerState::load_snapshot(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    return false; // no snapshot yet is not an error for the caller
  }

  std::unique_lock lock(mtx_);
  peers_.clear();
  groups_.clear();
  files_.clear();
  group_files_.clear();

  std::string line;
  long long line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    std::vector<std::string> t = split_args(line);
    if (t.empty()) {
      continue;
    }

    if (t[0] == "V") {
      continue;
    }

    if (t[0] == "USER") {
      if (t.size() != 4) return false;
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
      Group g;
      g.gid = t[1];
      g.groupmaster = (t[2] == "-") ? "" : t[2];
      size_t i = 3;
      long long n = 0;
      std::vector<std::string> items;
      if (!take_list(t, i, n, items)) return false;
      g.participants.insert(items.begin(), items.end());
      if (!take_list(t, i, n, items)) return false;
      g.applicants.insert(items.begin(), items.end());
      groups_[t[1]] = std::move(g);
      continue;
    }

    if (t[0] == "FILE") {
      if (t.size() < 5) return false;
      FileMeta f;
      if (!parse_ll(t[2], f.size) || f.size < 0) return false;
      f.fullhash = t[3];
      size_t i = 4;
      long long n = 0;
      std::vector<std::string> items;
      if (!take_list(t, i, n, items)) return false;
      f.piece_hashes = items;
      f.num_pieces = (int)items.size();
      if (!take_list(t, i, n, items)) return false;
      f.peers.insert(items.begin(), items.end());
      files_[t[1]] = std::move(f);
      continue;
    }

    if (t[0] == "GFILE") {
      if (t.size() < 3) return false;
      size_t i = 2;
      long long n = 0;
      std::vector<std::string> items;
      if (!take_list(t, i, n, items)) return false;
      group_files_[t[1]].insert(items.begin(), items.end());
      continue;
    }

    std::cerr << "snapshot: unknown record on line " << line_no << ": " << t[0]
              << std::endl;
    return false;
  }
  return true;
}

} // namespace p2p
