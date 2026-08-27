#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "client/upload_registry.h"

namespace p2p {

// Serves file pieces to other peers.
//
// Owns a listening socket and a small pool of worker threads. The accept
// loop hands accepted sockets to the pool through a queue rather than
// spawning a thread per request, so a burst of downloaders cannot create
// unbounded threads.
class PeerServer {
public:
  PeerServer(UploadRegistry &registry, int num_workers = 4);
  ~PeerServer();

  PeerServer(const PeerServer &) = delete;
  PeerServer &operator=(const PeerServer &) = delete;

  // Binds `ip`:`port` and starts the accept loop and worker pool.
  // Returns false if the socket could not be bound.
  bool start(const std::string &ip, const std::string &port);

  // Stops accepting, drains the pool and joins every thread.
  void stop();

private:
  void accept_loop();
  void worker_loop();
  void serve_one(int peersock);

  UploadRegistry &registry_;
  int num_workers_;
  int listen_fd_ = -1;

  std::thread accept_thread_;
  std::vector<std::thread> workers_;

  std::mutex mtx_;
  std::condition_variable cv_;
  std::queue<int> pending_;
  bool stopping_ = false;
};

} // namespace p2p
