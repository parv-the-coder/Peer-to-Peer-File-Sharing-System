#include "client/web_ui.h"

#include <sstream>
#include <vector>

#include "client/web_assets.h"
#include "common/message.h"

namespace p2p {
namespace {

// Pulls the strings out of {"args":["a","b"]}. Deliberately minimal: this
// endpoint accepts exactly one shape, so a full JSON parser would be more
// surface than the feature needs. Anything it does not understand yields
// no arguments, which execute() reports as an unrecognised command.
std::vector<std::string> parse_args(const std::string &body) {
  std::vector<std::string> out;
  size_t k = body.find("\"args\"");
  if (k == std::string::npos) return out;
  size_t open = body.find('[', k);
  size_t close = body.find(']', open == std::string::npos ? k : open);
  if (open == std::string::npos || close == std::string::npos) return out;

  size_t i = open + 1;
  while (i < close) {
    size_t q = body.find('"', i);
    if (q == std::string::npos || q >= close) break;
    std::string val;
    ++q;
    while (q < body.size() && body[q] != '"') {
      if (body[q] == '\\' && q + 1 < body.size()) {
        ++q;
        switch (body[q]) {
        case 'n': val += '\n'; break;
        case 't': val += '\t'; break;
        case 'r': val += '\r'; break;
        default:  val += body[q];
        }
      } else {
        val += body[q];
      }
      ++q;
    }
    out.push_back(val);
    i = q + 1;
  }
  return out;
}

// The tracker replies to list_groups with a banner line then one group per
// line; turn that into a JSON array for the page.
std::string groups_json(CommandProcessor &processor) {
  if (!processor.logged_in()) return "[]";
  std::string text = processor.execute({"list_groups"});
  std::ostringstream js;
  js << "[";
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    // skip the banner and any error text
    if (line.empty() || line.find('#') != std::string::npos ||
        line.find("ERROR") != std::string::npos ||
        line.find("---") != std::string::npos) {
      continue;
    }
    if (!first) js << ",";
    js << "\"" << json_escape(line) << "\"";
    first = false;
  }
  js << "]";
  return js.str();
}

} // namespace

void register_web_ui(HttpServer &server, CommandProcessor &processor,
                     Downloader &downloader, const std::string &peer_port) {
  server.route("GET", "/", [](const HttpRequest &) {
    HttpResponse r;
    r.content_type = "text/html; charset=utf-8";
    r.body = kDashboardHtml;
    return r;
  });

  server.route("GET", "/api/status",
               [&processor, &downloader, peer_port](const HttpRequest &) {
    std::ostringstream js;
    js << "{";
    js << "\"logged_in\":" << (processor.logged_in() ? "true" : "false");
    js << ",\"username\":\"" << json_escape(processor.username()) << "\"";
    js << ",\"peer_port\":\"" << json_escape(peer_port) << "\"";
    js << ",\"groups\":" << groups_json(processor);
    js << ",\"downloads\":[";
    bool first = true;
    for (const auto &d : downloader.snapshot()) {
      int failed = 0;
      for (int st : d.piece_status) {
        if (st == 3) ++failed;
      }
      if (!first) js << ",";
      js << "{\"filename\":\"" << json_escape(d.filename) << "\""
         << ",\"group\":\"" << json_escape(d.group_id) << "\""
         << ",\"total_size\":" << d.total_size
         << ",\"total_pieces\":" << d.total_pieces
         << ",\"completed_pieces\":" << d.completed_pieces
         << ",\"failed\":" << failed
         << ",\"active\":" << (d.is_active ? "true" : "false") << "}";
      first = false;
    }
    js << "]}";

    HttpResponse r;
    r.content_type = "application/json";
    r.body = js.str();
    return r;
  });

  server.route("POST", "/api/command", [&processor](const HttpRequest &req) {
    std::vector<std::string> args = parse_args(req.body);
    // execute() applies the same login and usage checks the REPL gets.
    std::string output = processor.execute(args);
    HttpResponse r;
    r.content_type = "application/json";
    r.body = "{\"output\":\"" + json_escape(output) + "\"}";
    return r;
  });
}

} // namespace p2p
