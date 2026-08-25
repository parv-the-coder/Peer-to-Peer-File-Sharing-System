#include "tracker/session_manager.h"

#include "common/auth.h"

namespace p2p {

std::string SessionManager::create(const std::string &username,
                                   const std::string &ip,
                                   const std::string &port) {
  std::string token = random_hex(kTokenBytes);
  if (token.empty()) {
    return "";
  }
  SessionInfo info;
  info.username = username;
  info.ip = ip;
  info.port = port;
  info.expires_at = std::chrono::steady_clock::now() + kSessionTtl;

  std::lock_guard<std::mutex> lock(mtx_);
  sessions_[token] = std::move(info);
  return token;
}

bool SessionManager::lookup(const std::string &token, SessionInfo &out) {
  if (token.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return false;
  }
  if (std::chrono::steady_clock::now() >= it->second.expires_at) {
    sessions_.erase(it);
    return false;
  }
  out = it->second;
  return true;
}

bool SessionManager::username_for(const std::string &token,
                                  std::string &username) {
  SessionInfo info;
  if (!lookup(token, info)) {
    return false;
  }
  username = info.username;
  return true;
}

std::string SessionManager::destroy(const std::string &token) {
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return "";
  }
  std::string username = it->second.username;
  sessions_.erase(it);
  return username;
}

void SessionManager::destroy_all_for(const std::string &username) {
  std::lock_guard<std::mutex> lock(mtx_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    it = (it->second.username == username) ? sessions_.erase(it) : std::next(it);
  }
}

} // namespace p2p
