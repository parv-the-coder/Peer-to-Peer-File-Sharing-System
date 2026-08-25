#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace p2p {

// How long a session token stays valid after it is issued.
constexpr std::chrono::seconds kSessionTtl{24 * 60 * 60};

struct SessionInfo {
  std::string username;
  std::string ip;   // taken from the socket, not from the client
  std::string port; // client-supplied listening port
  std::chrono::steady_clock::time_point expires_at;
};

// Maps opaque session tokens to the identity they authenticate.
//
// Kept separate from TrackerState with its own mutex: every privileged
// command validates a token, so putting that lookup behind TrackerState's
// lock would make it contend with slower operations like list_files.
//
// Tokens have a fixed TTL from issue time and are not refreshed on use.
// Refresh-on-use was considered and rejected: it means writing to the
// session map on every command, turning the hot read path into a write,
// for no benefit this project needs.
class SessionManager {
public:
  // Issues a token for `username`. Returns an empty string if the CSPRNG
  // is unavailable, which the caller must treat as a login failure.
  std::string create(const std::string &username, const std::string &ip,
                     const std::string &port);

  // Resolves a token to its session. Returns false if the token is
  // unknown or expired; an expired entry is dropped as a side effect.
  bool lookup(const std::string &token, SessionInfo &out);

  // Convenience: resolves a token straight to a username.
  bool username_for(const std::string &token, std::string &username);

  // Invalidates a single token (logout). Returns the username it
  // belonged to, or an empty string if the token was not valid.
  std::string destroy(const std::string &token);

  // Invalidates every token belonging to a user.
  void destroy_all_for(const std::string &username);

private:
  std::mutex mtx_;
  std::unordered_map<std::string, SessionInfo> sessions_;
};

} // namespace p2p
