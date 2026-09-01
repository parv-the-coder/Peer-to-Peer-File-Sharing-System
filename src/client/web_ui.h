#pragma once

#include <string>

#include "client/commands.h"
#include "client/downloader.h"
#include "client/http_server.h"

namespace p2p {

// Registers the dashboard and its two endpoints on `server`:
//
//   GET  /              the page
//   GET  /api/status    session, groups and download progress as JSON
//   POST /api/command   {"args":[...]} -> {"output":"..."}
//
// Every action goes through CommandProcessor::execute, the same function
// the REPL calls, so the interface cannot perform anything the REPL would
// refuse -- including anything at all before login.
void register_web_ui(HttpServer &server, CommandProcessor &processor,
                     Downloader &downloader, const std::string &peer_port);

} // namespace p2p
