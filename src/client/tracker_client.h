#pragma once

#include <mutex>
#include <string>

namespace p2p {

// The client's connection to the tracker.
//
// Owns the socket and the session token issued at login. send() holds a
// mutex across the whole request/response pair: the framed protocol is
// strictly one reply per request, so two threads interleaving on the
// same socket would each receive the other's response.
class TrackerClient {
public:
  ~TrackerClient();

  bool connect_to(const std::string &ip, const std::string &port);
  void disconnect();
  bool connected() const { return sock_ >= 0; }

  // Sends a framed command and returns the framed reply. Empty string on
  // a transport failure.
  std::string send(const std::string &cmd);

  void set_token(const std::string &t) { token_ = t; }
  const std::string &token() const { return token_; }
  void clear_token() { token_.clear(); }

private:
  int sock_ = -1;
  std::string token_;
  std::mutex mtx_;
};

} // namespace p2p
