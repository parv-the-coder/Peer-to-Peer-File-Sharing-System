#include "client/peer_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>

#include "common/hash.h"
#include "common/message.h"
#include "common/socket_io.h"

namespace p2p {

PeerServer::PeerServer(UploadRegistry &registry, int num_workers)
    : registry_(registry), num_workers_(num_workers) {}

PeerServer::~PeerServer() { stop(); }

bool PeerServer::start(const std::string &ip, const std::string &port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("------- Failed to create socket -------");
    return false;
  }

  // SO_REUSEADDR only, not SO_REUSEPORT: two clients sharing one peer
  // port would have the kernel split GET_PIECE requests between them,
  // and only one of them actually has the file.
  int choice = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &choice, sizeof(choice))) {
    perror("------- Failed to set socket options -------");
    close(sock);
    return false;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  // Honour the address the user actually asked for rather than binding
  // every interface regardless.
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    std::cout << "------- Invalid listening address: " << ip << " -------"
              << std::endl;
    close(sock);
    return false;
  }
  int listen_port = 0;
  if (!parse_int(port, listen_port) || listen_port <= 0 || listen_port > 65535) {
    std::cout << "------- Invalid listening port: " << port << " -------"
              << std::endl;
    close(sock);
    return false;
  }
  addr.sin_port = htons(static_cast<uint16_t>(listen_port));

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("Bind Error");
    std::cout << "Is another client already using peer port " << port << "?"
              << std::endl;
    close(sock);
    return false;
  }
  if (listen(sock, 20) < 0) {
    perror("listen");
    close(sock);
    return false;
  }

  listen_fd_ = sock;
  for (int i = 0; i < num_workers_; ++i) {
    workers_.emplace_back(&PeerServer::worker_loop, this);
  }
  accept_thread_ = std::thread(&PeerServer::accept_loop, this);
  return true;
}

void PeerServer::accept_loop() {
  // Cache the descriptor. listen_fd_ is written once in start() before
  // this thread exists and again in stop() after it has been joined, so
  // reading it here in the loop would be a race with that second write.
  const int listener = listen_fd_;
  while (true) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(listener, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
      std::lock_guard<std::mutex> lock(mtx_);
      if (stopping_) {
        return;
      }
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (stopping_) {
        close(fd);
        return;
      }
      pending_.push(fd);
    }
    cv_.notify_one();
  }
}

void PeerServer::worker_loop() {
  while (true) {
    int fd;
    {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_.wait(lock, [this] { return !pending_.empty() || stopping_; });
      if (stopping_ && pending_.empty()) {
        return;
      }
      fd = pending_.front();
      pending_.pop();
    }
    serve_one(fd);
  }
}

void PeerServer::serve_one(int peersock) {
  std::string request;
  if (!recv_framed(peersock, request)) {
    close(peersock);
    return;
  }

  std::vector<std::string> args = split_args(request);
  if (args.empty() || args[0] != "GET_PIECE" || args.size() < 3) {
    close(peersock);
    return;
  }

  const std::string &fname = args[1];
  int index = 0;
  // Peer-supplied: stoi would throw on garbage and, on a worker thread,
  // terminate the whole client.
  if (!parse_int(args[2], index) || index < 0) {
    close(peersock);
    return;
  }

  std::string fullpath;
  if (!registry_.path_for(fname, fullpath)) {
    close(peersock);
    return;
  }

  int fd = open(fullpath.c_str(), O_RDONLY);
  if (fd < 0) {
    close(peersock);
    return;
  }

  std::vector<char> buf(kPieceSize);
  off_t offset = static_cast<off_t>(index) * static_cast<off_t>(kPieceSize);
  ssize_t n = pread(fd, buf.data(), kPieceSize, offset);
  close(fd);

  if (n > 0) {
    // Piece payload is framed like every other message: [4-byte len][data]
    send_framed(peersock, std::string(buf.data(), static_cast<size_t>(n)));
  }
  close(peersock);
}

void PeerServer::stop() {
  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  cv_.notify_all();

  // shutdown() wakes the blocked accept() without closing the
  // descriptor. The close has to wait until the accept thread has been
  // joined: closing an fd another thread is still blocked on invites
  // that number being reused for a new connection underneath it.
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
  for (auto &t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
  workers_.clear();

  std::lock_guard<std::mutex> lock(mtx_);
  while (!pending_.empty()) {
    close(pending_.front());
    pending_.pop();
  }
}

} // namespace p2p
