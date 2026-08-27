#pragma once

#include <mutex>
#include <string>
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

  // Runs one download to completion. Blocks until every piece is fetched
  // or retries are exhausted.
  void download(const std::string &gid, const std::string &fname,
                const std::string &destpath);

  // Human-readable summary of active and completed downloads.
  std::string report() const;

private:
  TrackerClient &tracker_;
  UploadRegistry &registry_;

  mutable std::mutex mtx_;
  std::unordered_map<std::string, DownloadInfo> downloads_;
};

} // namespace p2p
