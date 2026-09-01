#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "client/downloader.h"
#include "client/tracker_client.h"
#include "client/upload_registry.h"

namespace p2p {

// Executes one user command and returns its output as text.
//
// This is the single place a command is interpreted. The REPL and the web
// interface both call execute(), so neither can drift from the other and
// there is no second copy of the login checks to keep in step -- a web
// request cannot reach an action the CLI would have refused.
//
// Safe to call from several threads: session state is guarded, and the
// collaborators it calls into (TrackerClient, Downloader, UploadRegistry)
// each do their own locking.
class CommandProcessor {
public:
  CommandProcessor(TrackerClient &tracker, UploadRegistry &registry,
                   Downloader &downloader, std::string host_port);

  // args[0] is the verb. Returns the text the user should see.
  std::string execute(const std::vector<std::string> &args);

  bool logged_in() const;
  std::string username() const;

  // Text listing of the available commands.
  static std::string help();

private:
  // Requires a session; returns an error string when not logged in.
  bool require_login(std::string &err) const;
  void set_session(const std::string &user, const std::string &token);
  void clear_session();

  TrackerClient &tracker_;
  UploadRegistry &registry_;
  Downloader &downloader_;
  std::string host_port_;

  mutable std::mutex mtx_;
  std::string peername_;
  bool connected_ = false;
};

} // namespace p2p
