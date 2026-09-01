#include "client/commands.h"

#include <fcntl.h>
#include <unistd.h>

#include <sstream>

#include "common/hash.h"

namespace p2p {

CommandProcessor::CommandProcessor(TrackerClient &tracker,
                                   UploadRegistry &registry,
                                   Downloader &downloader,
                                   std::string host_port)
    : tracker_(tracker), registry_(registry), downloader_(downloader),
      host_port_(std::move(host_port)) {}

bool CommandProcessor::logged_in() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return connected_;
}

std::string CommandProcessor::username() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return peername_;
}

bool CommandProcessor::require_login(std::string &err) const {
  std::lock_guard<std::mutex> lock(mtx_);
  if (!connected_) {
    err = "------- You must log in first. ---------\n";
    return false;
  }
  return true;
}

void CommandProcessor::set_session(const std::string &user,
                                   const std::string &token) {
  std::lock_guard<std::mutex> lock(mtx_);
  peername_ = user;
  connected_ = true;
  tracker_.set_token(token);
}

void CommandProcessor::clear_session() {
  std::lock_guard<std::mutex> lock(mtx_);
  peername_.clear();
  connected_ = false;
  tracker_.clear_token();
}

std::string CommandProcessor::help() {
  return "\n==================== Available Commands ====================\n"
         "create_user <username> <password>\n"
         "login <username> <password>\n"
         "logout\n"
         "create_group <groupid>\n"
         "join_group <groupid>\n"
         "leave_group <groupid>\n"
         "list_requests <groupid>\n"
         "accept_request <groupid> <username>\n"
         "reject_request <groupid> <username>\n"
         "list_groups\n"
         "list_files <groupid>\n"
         "upload_file <groupid> <filepath>\n"
         "download_file <groupid> <filename> <dest_path>\n"
         "stop_share <groupid> <filename>\n"
         "show_downloads\n"
         "commands\n"
         "exit\n"
         "============================================================\n\n";
}

std::string CommandProcessor::execute(const std::vector<std::string> &args) {
  if (args.empty()) {
    return "-------- Unrecognized command. Enter a valid command. --------\n";
  }
  const std::string &cmd = args[0];
  const size_t n = args.size();
  std::string err;

  if (cmd == "commands") {
    return help();
  }

  // --- no session required ---

  if (cmd == "create_user") {
    if (n != 3) return "Usage: create_user <user> <pass>\n";
    return tracker_.send("create_user " + args[1] + " " + args[2]) + "\n";
  }

  if (cmd == "login") {
    if (n != 3) return "Usage: login <user> <pass>\n";
    if (logged_in()) return "-------- User session already active --------\n";
    // Only the listening port is sent: the tracker takes our IP from the
    // socket rather than trusting what we claim.
    std::string r = tracker_.send("login " + args[1] + " " + args[2] + " " + host_port_);
    if (r.rfind("OK ", 0) == 0) {
      clear_session();
      set_session(args[1], r.substr(3));
      return "********* You are now logged in *********\n";
    }
    return r + "\n";
  }

  // --- everything below needs a session ---

  if (!require_login(err)) {
    return err;
  }

  if (cmd == "logout") {
    std::string r = tracker_.send("logout " + tracker_.token());
    clear_session();
    return r + "\n";
  }

  if (cmd == "create_group") {
    if (n != 2) return "Usage: create_group <groupid>\n";
    return tracker_.send("create_group " + tracker_.token() + " " + args[1]) + "\n";
  }
  if (cmd == "join_group") {
    if (n != 2) return "Usage: join_group <groupid>\n";
    return tracker_.send("join_group " + tracker_.token() + " " + args[1]) + "\n";
  }
  if (cmd == "leave_group") {
    if (n != 2) return "Usage: leave_group <groupid>\n";
    return tracker_.send("leave_group " + tracker_.token() + " " + args[1]) + "\n";
  }
  if (cmd == "list_requests") {
    if (n != 2) return "Usage: list_requests <groupid>\n";
    return tracker_.send("list_requests " + tracker_.token() + " " + args[1]) + "\n";
  }
  if (cmd == "accept_request") {
    if (n != 3) return "Usage: accept_request <groupid> <user>\n";
    return tracker_.send("accept_request " + tracker_.token() + " " + args[1] +
                         " " + args[2]) + "\n";
  }
  if (cmd == "reject_request") {
    if (n != 3) return "Usage: reject_request <groupid> <user>\n";
    return tracker_.send("reject_request " + tracker_.token() + " " + args[1] +
                         " " + args[2]) + "\n";
  }
  if (cmd == "list_groups") {
    return tracker_.send("list_groups " + tracker_.token()) + "\n";
  }
  if (cmd == "list_files") {
    if (n != 2) return "Usage: list_files <groupid>\n";
    return tracker_.send("list_files " + tracker_.token() + " " + args[1]) + "\n";
  }

  if (cmd == "upload_file") {
    if (n < 3) return "Usage: upload_file <groupid> <filepath>\n";
    const std::string &gid = args[1];
    // A path may contain spaces, so everything after the group id is
    // rejoined rather than taking a single token.
    std::string fpath;
    for (size_t i = 2; i < n; ++i) {
      if (i > 2) fpath += " ";
      fpath += args[i];
    }
    int fd = open(fpath.c_str(), O_RDONLY);
    if (fd < 0) return "File not found: " + fpath + "\n";
    off_t fsize = lseek(fd, 0, SEEK_END);
    close(fd);
    if (fsize == 0) return "Cannot upload empty file: " + fpath + "\n";

    long long num_pieces = 0;
    std::vector<std::string> piece_hashes = sha1_file_pieces(fpath, num_pieces);
    std::string fullhash = sha1_file_hex(fpath);
    size_t pos = fpath.find_last_of('/');
    std::string fname = (pos == std::string::npos) ? fpath : fpath.substr(pos + 1);
    // Register locally first so the peer server can serve pieces the
    // moment the tracker starts handing our address to downloaders.
    registry_.add(fname, fpath);

    std::string msg = "upload_file " + tracker_.token() + " " + gid + " " +
                      fname + " " + std::to_string(fsize) + " " + fullhash +
                      " " + std::to_string(num_pieces);
    for (const auto &h : piece_hashes) msg += " " + h;
    return tracker_.send(msg) + "\n";
  }

  if (cmd == "download_file") {
    if (n < 4) return "Usage: download_file <groupid> <filename> <dest_path>\n";
    // Returns immediately; the transfer runs on its own thread so several
    // files can download at once and the prompt stays live.
    if (downloader_.start(args[1], args[2], args[3])) {
      return "Download started in background. Use show_downloads for progress.\n";
    }
    return "";
  }

  if (cmd == "stop_share") {
    if (n != 3) return "Usage: stop_share <groupid> <filename>\n";
    const std::string &gid = args[1], &fname = args[2];
    std::string out;
    if (registry_.contains(fname)) {
      registry_.remove(fname);
      out += "Stopped sharing file: " + fname + " in group " + gid + "\n";
    } else {
      out += "File " + fname + " is not being shared by you.\n";
    }
    out += tracker_.send("stop_share " + tracker_.token() + " " + gid + " " + fname) + "\n";
    return out;
  }

  if (cmd == "show_downloads") {
    return downloader_.report();
  }

  return "------- Invalid Command --------\n";
}

} // namespace p2p
