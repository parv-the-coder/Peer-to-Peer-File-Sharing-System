#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "client/tracker_client.h"
#include "client/upload_registry.h"

namespace p2p {

// Per-piece state of one in-progress download.
struct DownloadInfo {
  std::string group_id;
  std::string filename;
  std::string dest_path;
  long long total_size = 0;
  long long total_pieces = 0;
  long long completed_pieces = 0;
  // 0 = pending, 1 = downloading, 2 = completed, 3 = failed
  std::vector<int> piece_status;
  std::vector<std::string> piece_hashes;
  std::string full_hash;
  bool is_active = false;
};

// Fetches files piece-by-piece from peers.
//
// Pieces are pulled in parallel by a pool of worker threads, each of
// which starts at a different peer so that concurrent workers spread
// across the swarm instead of all hammering the first one. Every piece
// is verified against its expected SHA-1 before being written, and the
// assembled file is verified against the full-file hash at the end.
//
// Progress is persisted to a "<file>.downloading" sidecar after each
// piece, so an interrupted download resumes rather than restarting.
class Downloader {
public:
  Downloader(TrackerClient &tracker, UploadRegistry &registry)
      : tracker_(tracker), registry_(registry) {}

  ~Downloader();

  // Starts a download on its own thread and returns immediately, so the
  // REPL stays responsive and several files can be fetched at once.
  // Returns false if that file is already downloading.
  bool start(const std::string &gid, const std::string &fname,
             const std::string &destpath);

  // Human-readable summary of active and completed downloads.
  std::string report() const;

  // Snapshot of every tracked download, for the web interface. Returns a
  // copy rather than a reference so callers never read the live map
  // without the lock.
  std::vector<DownloadInfo> snapshot() const;

  // Joins every outstanding download thread. Called on exit.
  void wait_all();

private:
  // The actual transfer. Runs on a background thread.
  void run(const std::string &gid, const std::string &fname,
           const std::string &destpath);
  void mark_inactive(const std::string &fname);

  TrackerClient &tracker_;
  UploadRegistry &registry_;

  mutable std::mutex mtx_;
  std::unordered_map<std::string, DownloadInfo> downloads_;

  // Threads are kept so they can be joined at shutdown rather than
  // detached: a detached download still touching the registry or the
  // tracker socket while main() tears them down is a use-after-free.
  std::mutex threads_mtx_;
  std::vector<std::thread> threads_;
};

} // namespace p2p
