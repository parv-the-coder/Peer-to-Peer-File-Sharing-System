#include "tracker/tracker_state.h"

#include <mutex>

namespace p2p {

void Peer::login(const std::string &ip, const std::string &port) {
  hostip = ip;
  hostport = port;
  connected = true;
}

void Peer::logout() {
  connected = false;
  hostip.clear();
  hostport.clear();
}

bool Group::isapplicant(const std::string &s) const {
  return applicants.find(s) != applicants.end();
}

bool Group::partofgroup(const std::string &s) const {
  return participants.find(s) != participants.end();
}

void Group::acceptreq(const std::string &s) {
  applicants.erase(s);
  participants.insert(s);
}

void Group::deluser(const std::string &s) {
  participants.erase(s);
  if (s != groupmaster) {
    return;
  }
  if (!participants.empty()) {
    groupmaster = *participants.begin();
  } else {
    groupmaster = "";
  }
}

bool TrackerState::user_exists_locked(const std::string &name) const {
  return peers_.find(name) != peers_.end();
}

bool TrackerState::group_exists_locked(const std::string &gid) const {
  return groups_.find(gid) != groups_.end();
}

bool TrackerState::group_has_file_locked(const std::string &gid,
                                         const std::string &fname) const {
  auto it = group_files_.find(gid);
  return it != group_files_.end() && it->second.find(fname) != it->second.end();
}

Result TrackerState::create_user(const std::string &username,
                                 const std::string &passcode) {
  std::unique_lock lock(mtx_);
  if (user_exists_locked(username)) {
    return {false, "-----Cannot create user: ID already in use.-----"};
  }
  PasswordHash ph = hash_password(passcode);
  if (ph.salt.empty() || ph.hash.empty()) {
    return {false, "-----Cannot create user: server entropy unavailable-----"};
  }
  Peer peer;
  peer.peername = username;
  peer.password = std::move(ph);
  peers_[username] = std::move(peer);
  return {true,
          "***** ID number " + username + " registered successfully! ******"};
}

Result TrackerState::login(const std::string &username,
                           const std::string &passcode, const std::string &ip,
                           const std::string &port) {
  std::unique_lock lock(mtx_);
  auto it = peers_.find(username);
  if (it == peers_.end()) {
    return {false, "------ User ID " + username + " is not registered ------"};
  }
  if (!verify_password(passcode, it->second.password)) {
    return {false, "------ Authentication failed: incorrect passcode for ID " +
                       username + " ------"};
  }
  it->second.login(ip, port);
  return {true, "Successful Login for User ID " + username + "! ******\n"};
}

Result TrackerState::logout(const std::string &username) {
  std::unique_lock lock(mtx_);
  auto it = peers_.find(username);
  if (it == peers_.end()) {
    return {false, "------- No such User ID: " + username + " ------"};
  }
  it->second.logout();
  return {true,
          "***** User ID " + username + " logged out successfully ******"};
}

Result TrackerState::create_group(const std::string &gid,
                                  const std::string &owner) {
  std::unique_lock lock(mtx_);
  if (!user_exists_locked(owner)) {
    return {false, "------- No such User ID: " + owner + " ------"};
  }
  if (group_exists_locked(gid)) {
    return {false, "------- This Group ID is already taken ------"};
  }
  Group grp;
  grp.gid = gid;
  grp.groupmaster = owner;
  grp.participants.insert(owner);
  groups_[gid] = std::move(grp);
  return {true,
          "******* Group creation successful. Assigned ID: " + gid + " *******"};
}

Result TrackerState::join_group(const std::string &gid,
                                const std::string &username) {
  std::unique_lock lock(mtx_);
  if (!user_exists_locked(username)) {
    return {false, "------- No such User ID: " + username + " ------"};
  }
  auto it = groups_.find(gid);
  if (it == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (it->second.partofgroup(username)) {
    return {false,
            "------- You have already joined this group: " + gid + " -------"};
  }
  it->second.applicants.insert(username);
  return {true,
          "******* Request to join group " + gid + " has been sent ******"};
}

Result TrackerState::leave_group(const std::string &gid,
                                 const std::string &username) {
  std::unique_lock lock(mtx_);
  if (!user_exists_locked(username)) {
    return {false, "------- No such User ID: " + username + " ------"};
  }
  auto it = groups_.find(gid);
  if (it == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (!it->second.partofgroup(username)) {
    return {false, "------ Access denied. You are not part of Group ID " + gid +
                       " -------"};
  }
  it->second.deluser(username);
  return {true, "****** Left group successfully. ID: " + gid + " ******"};
}

Result TrackerState::list_requests(const std::string &gid,
                                   const std::string &owner) const {
  std::shared_lock lock(mtx_);
  if (!user_exists_locked(owner)) {
    return {false, "------- No such User ID: " + owner + " ------"};
  }
  auto it = groups_.find(gid);
  if (it == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (it->second.groupmaster != owner) {
    return {false, "------ Access denied. You are not the group owner of ID " +
                       gid + " -------"};
  }
  std::string msg;
  for (const auto &user : it->second.applicants) {
    msg += user + "\n";
  }
  if (msg.empty()) {
    msg = "------- Group ID " + gid + " has no pending join requests -------";
  }
  return {true, msg};
}

Result TrackerState::accept_request(const std::string &gid,
                                    const std::string &applicant,
                                    const std::string &owner) {
  std::unique_lock lock(mtx_);
  if (!user_exists_locked(applicant)) {
    return {false, "------- No such User ID: " + applicant + " ------"};
  }
  auto it = groups_.find(gid);
  if (it == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (it->second.groupmaster != owner) {
    return {false, "------ Access denied. You are not the group owner of ID " +
                       gid + " -------"};
  }
  if (!it->second.isapplicant(applicant)) {
    return {false, "------- This user (ID: " + applicant +
                       ") has no pending requests -------"};
  }
  it->second.acceptreq(applicant);
  return {true, "******* Approval granted for User ID: " + applicant +
                    " *******"};
}

Result TrackerState::reject_request(const std::string &gid,
                                    const std::string &applicant,
                                    const std::string &owner) {
  std::unique_lock lock(mtx_);
  if (!user_exists_locked(applicant)) {
    return {false, "------- No such User ID: " + applicant + " ------"};
  }
  auto it = groups_.find(gid);
  if (it == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (it->second.groupmaster != owner) {
    return {false, "------ Access denied. You are not the group owner of ID " +
                       gid + " -------"};
  }
  if (!it->second.isapplicant(applicant)) {
    return {false, "------- This user (ID: " + applicant +
                       ") has no pending requests -------"};
  }
  it->second.applicants.erase(applicant);
  return {true, "******* Request rejected for User ID: " + applicant +
                    " *******"};
}

Result TrackerState::list_groups() const {
  std::shared_lock lock(mtx_);
  std::string msg =
      "############### Available groups on the network ###############";
  for (const auto &entry : groups_) {
    msg += "\n" + entry.first;
  }
  return {true, msg};
}

Result TrackerState::upload_file(const std::string &gid,
                                 const std::string &fname,
                                 const std::string &uname, long long size,
                                 const std::string &fullhash, int num_pieces,
                                 const std::vector<std::string> &piece_hashes) {
  std::unique_lock lock(mtx_);
  auto git = groups_.find(gid);
  if (git == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (!user_exists_locked(uname) || !git->second.partofgroup(uname)) {
    return {false, "------ You are not part of Group ID " + gid + " -------"};
  }
  FileMeta &fm = files_[fname];
  fm.size = size;
  fm.fullhash = fullhash;
  fm.num_pieces = num_pieces;
  fm.piece_hashes = piece_hashes;
  fm.piece_hashes.resize(static_cast<size_t>(num_pieces));
  fm.peers.insert(uname);
  group_files_[gid].insert(fname);
  return {true, "******* File " + fname + " uploaded to group " + gid +
                    " successfully *******"};
}

Result TrackerState::list_files(const std::string &gid,
                                const std::string &uname) const {
  std::shared_lock lock(mtx_);
  auto git = groups_.find(gid);
  if (git == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (!user_exists_locked(uname) || !git->second.partofgroup(uname)) {
    return {false, "------ Access denied. You are not part of Group ID " + gid +
                       " -------"};
  }
  auto fit = group_files_.find(gid);
  if (fit == group_files_.end() || fit->second.empty()) {
    return {true, "------- No files uploaded in group " + gid + " -------"};
  }
  std::string msg = "######## Files in Group " + gid + " ########\n";
  for (const auto &fname : fit->second) {
    auto mit = files_.find(fname);
    if (mit == files_.end()) {
      continue;
    }
    msg += fname + " SIZE:" + std::to_string(mit->second.size) +
           " PIECES:" + std::to_string(mit->second.num_pieces) + "\n";
  }
  return {true, msg};
}

Result TrackerState::download_file(const std::string &gid,
                                   const std::string &fname,
                                   const std::string &uname) const {
  std::shared_lock lock(mtx_);
  auto git = groups_.find(gid);
  if (git == groups_.end()) {
    return {false, "------- No such group ID: " + gid + " ------"};
  }
  if (!user_exists_locked(uname) || !git->second.partofgroup(uname)) {
    return {false, "------ Access denied. You are not part of Group ID " + gid +
                       " -------"};
  }
  if (!group_has_file_locked(gid, fname)) {
    return {false, "------- No such file in group " + gid + " -------"};
  }
  auto mit = files_.find(fname);
  if (mit == files_.end()) {
    return {false, "------- No such file in group " + gid + " -------"};
  }
  const FileMeta &fm = mit->second;
  std::string msg = "FILE " + fname + " SIZE " + std::to_string(fm.size) +
                    " HASH " + fm.fullhash + " PIECES " +
                    std::to_string(fm.num_pieces) + " PIECE_HASHES";
  for (const auto &h : fm.piece_hashes) {
    msg += " " + h;
  }
  msg += "\nPEERS\n";
  for (const std::string &peer : fm.peers) {
    auto pit = peers_.find(peer);
    if (pit != peers_.end() && pit->second.connected) {
      msg += peer + " " + pit->second.hostip + " " + pit->second.hostport + "\n";
    }
  }
  msg += "\n";
  return {true, msg};
}

Result TrackerState::file_downloaded(const std::string &gid,
                                     const std::string &fname,
                                     const std::string &peername) {
  std::unique_lock lock(mtx_);
  auto git = groups_.find(gid);
  if (git == groups_.end() || !git->second.partofgroup(peername)) {
    return {false, "ERROR: Group not found or peer not member"};
  }
  if (!group_has_file_locked(gid, fname)) {
    return {false, "ERROR: File not found in group"};
  }
  auto pit = peers_.find(peername);
  if (pit == peers_.end()) {
    return {false, "ERROR: Peer not found"};
  }
  auto mit = files_.find(fname);
  if (mit != files_.end()) {
    mit->second.peers.insert(peername);
  }
  return {true, "SUCCESS: Peer " + peername + " registered as seeder for " +
                    fname};
}

Result TrackerState::stop_share(const std::string &gid,
                                const std::string &fname,
                                const std::string &peername) {
  std::unique_lock lock(mtx_);
  auto git = groups_.find(gid);
  if (git == groups_.end()) {
    return {false, "ERROR: Group not found"};
  }
  if (!user_exists_locked(peername) || !git->second.partofgroup(peername)) {
    return {false, "ERROR: Peer not found or not member of group"};
  }
  if (!group_has_file_locked(gid, fname)) {
    return {false, "ERROR: File not found in group"};
  }
  auto mit = files_.find(fname);
  if (mit == files_.end()) {
    return {false, "ERROR: File metadata not found"};
  }
  mit->second.peers.erase(peername);
  return {true, "SUCCESS: Peer " + peername + " stopped sharing " + fname +
                    " in group " + gid};
}

void TrackerState::handle_disconnect(const std::string &username) {
  if (username.empty()) {
    return;
  }
  std::unique_lock lock(mtx_);
  auto pit = peers_.find(username);
  if (pit != peers_.end()) {
    pit->second.logout();
  }
  // Group ownership is deliberately NOT handed off here. A dropped
  // connection is transient -- the owner restarting their client should
  // not permanently lose the group, which is what the previous behaviour
  // did. Ownership moves only on an explicit leave_group.
  //
  // The peer also stays in each file's seeder set: it still holds those
  // files, it is merely offline. download_file filters seeders by their
  // connected flag, so clearing that flag is what actually stops the
  // tracker handing out a dead address -- which it previously did,
  // making downloaders burn their full retry budget against a peer that
  // was gone.
}

} // namespace p2p
