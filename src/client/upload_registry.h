#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

namespace p2p {

// Maps a shared filename to its path on local disk.
//
// This is shared between the CLI thread (which adds entries on
// upload_file and on a completed download, and removes them on
// stop_share) and every peer-serving worker thread (which reads them to
// answer GET_PIECE). The map it replaces was a bare unordered_map with
// no synchronisation at all -- ThreadSanitizer reports the resulting
// races on any run where a download is served while another upload is
// registered.
class UploadRegistry {
public:
  void add(const std::string &filename, const std::string &path);
  void remove(const std::string &filename);
  // Returns false if the file is not currently shared.
  bool path_for(const std::string &filename, std::string &path) const;
  bool contains(const std::string &filename) const;

private:
  mutable std::mutex mtx_;
  std::unordered_map<std::string, std::string> files_;
};

} // namespace p2p
