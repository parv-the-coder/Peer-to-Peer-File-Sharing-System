#include "client/upload_registry.h"

namespace p2p {

void UploadRegistry::add(const std::string &filename, const std::string &path) {
  std::lock_guard<std::mutex> lock(mtx_);
  files_[filename] = path;
}

void UploadRegistry::remove(const std::string &filename) {
  std::lock_guard<std::mutex> lock(mtx_);
  files_.erase(filename);
}

bool UploadRegistry::path_for(const std::string &filename,
                              std::string &path) const {
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = files_.find(filename);
  if (it == files_.end()) {
    return false;
  }
  path = it->second;
  return true;
}

bool UploadRegistry::contains(const std::string &filename) const {
  std::lock_guard<std::mutex> lock(mtx_);
  return files_.find(filename) != files_.end();
}

} // namespace p2p
