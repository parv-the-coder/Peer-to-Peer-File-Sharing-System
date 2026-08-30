#include "tracker/peer_link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <iostream>

#include "common/message.h"
#include "common/socket_io.h"

namespace p2p {
namespace {

constexpr auto kRetryDelay = std::chrono::seconds(2);
constexpr auto kPushInterval = std::chrono::milliseconds(500);

int dial(const Endpoint &ep) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return -1;
  }
  // Don't let a hung peer block this thread forever.
  struct timeval tv = {5, 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  int port = 0;
  if (!parse_int(ep.port, port) || port <= 0 || port > 65535) {
    close(sock);
    return -1;
  }
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ep.ip.c_str(), &addr.sin_addr) <= 0) {
    close(sock);
    return -1;
  }
  if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

} // namespace

PeerLink::PeerLink(TrackerState &state, const Endpoint &peer)
    : state_(state), peer_(peer) {}

PeerLink::~PeerLink() { stop(); }

void PeerLink::start() {
  running_ = true;
  thread_ = std::thread(&PeerLink::loop, this);
}

void PeerLink::stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void PeerLink::loop() {
  while (running_) {
    if (!session()) {
      connected_ = false;
      // Peer is down or unreachable. This tracker keeps serving clients
      // on its own; we just retry the link.
      for (int i = 0; i < 20 && running_; ++i) {
        std::this_thread::sleep_for(kRetryDelay / 20);
      }
    }
  }
}

bool PeerLink::session() {
  int sock = dial(peer_);
  if (sock < 0) {
    return false;
  }

  // Catch-up on connect: pull the peer's full state and merge it, so a
  // tracker that was offline picks up everything it missed. Because the
  // merge is a union and the peer performs the same exchange from its
  // side, both converge without either being authoritative.
  if (!send_framed(sock, "SYNC_REQ")) {
    close(sock);
    return false;
  }
  std::string theirs;
  if (!recv_framed(sock, theirs)) {
    close(sock);
    return false;
  }
  if (!state_.merge_from(theirs)) {
    std::cerr << "peer sync: rejected malformed state from peer" << std::endl;
  }

  if (!connected_.exchange(true)) {
    std::cout << "[sync] linked with tracker at " << peer_.ip << ":"
              << peer_.port << std::endl;
  }
  // Force a push straight after a merge so the peer sees anything we hold
  // that it does not.
  last_pushed_version_ = 0;

  while (running_) {
    unsigned long long v = state_.version();
    if (v != last_pushed_version_) {
      if (!send_framed(sock, "STATE " + state_.serialize())) {
        break;
      }
      std::string ack;
      if (!recv_framed(sock, ack)) {
        break;
      }
      last_pushed_version_ = v;
    }
    std::this_thread::sleep_for(kPushInterval);
  }

  close(sock);
  connected_ = false;
  std::cout << "[sync] link to " << peer_.ip << ":" << peer_.port << " lost"
            << std::endl;
  return false;
}

} // namespace p2p
