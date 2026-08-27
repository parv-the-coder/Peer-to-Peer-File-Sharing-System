#include "client/tracker_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>

#include "common/message.h"
#include "common/socket_io.h"

namespace p2p {

TrackerClient::~TrackerClient() { disconnect(); }

bool TrackerClient::connect_to(const std::string &ip, const std::string &port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    std::cout << "-------- Unable to create socket -------" << std::endl;
    return false;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  int p = 0;
  if (!parse_int(port, p) || p <= 0 || p > 65535) {
    std::cout << "Invalid tracker port: " << port << std::endl;
    close(sock);
    return false;
  }
  addr.sin_port = htons(static_cast<uint16_t>(p));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
    std::cout << "------- Error: Unable to parse address -------" << std::endl;
    close(sock);
    return false;
  }
  if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    std::cout << "-------- Failed to establish socket connection --------"
              << std::endl;
    close(sock);
    return false;
  }
  sock_ = sock;
  return true;
}

void TrackerClient::disconnect() {
  if (sock_ >= 0) {
    close(sock_);
    sock_ = -1;
  }
}

std::string TrackerClient::send(const std::string &cmd) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (sock_ < 0) {
    return "";
  }
  if (!send_framed(sock_, cmd)) {
    std::cout << "------- Failed to send command to tracker -------"
              << std::endl;
    return "";
  }
  std::string resp;
  if (!recv_framed(sock_, resp)) {
    std::cout << "------- Failed to read response from tracker -------"
              << std::endl;
    return "";
  }
  return resp;
}

} // namespace p2p
