#include "socket_io.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace p2p {

bool recv_all(int fd, void *buf, size_t n) {
  size_t total = 0;
  char *p = static_cast<char *>(buf);
  while (total < n) {
    ssize_t r = read(fd, p + total, n - total);
    if (r <= 0) {
      return false;
    }
    total += static_cast<size_t>(r);
  }
  return true;
}

bool send_all(int fd, const void *buf, size_t n) {
  size_t total = 0;
  const char *p = static_cast<const char *>(buf);
  while (total < n) {
    // MSG_NOSIGNAL is not optional here. Writing to a socket whose peer
    // has gone away raises SIGPIPE, whose default action is to terminate
    // the process -- so a peer disconnecting at the wrong moment killed
    // the whole tracker or client rather than failing this one write.
    // With the flag the call returns EPIPE instead and the caller sees a
    // false return, which every caller already handles.
    ssize_t s = send(fd, p + total, n - total, MSG_NOSIGNAL);
    if (s <= 0) {
      return false;
    }
    total += static_cast<size_t>(s);
  }
  return true;
}

bool send_framed(int fd, const std::string &payload) {
  uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
  if (!send_all(fd, &len, sizeof(len))) {
    return false;
  }
  return send_all(fd, payload.data(), payload.size());
}

bool recv_framed(int fd, std::string &out, size_t max_len) {
  uint32_t len_net = 0;
  if (!recv_all(fd, &len_net, sizeof(len_net))) {
    return false;
  }
  size_t len = ntohl(len_net);
  if (len > max_len) {
    return false;
  }
  out.resize(len);
  if (len == 0) {
    return true;
  }
  return recv_all(fd, out.data(), len);
}

} // namespace p2p
