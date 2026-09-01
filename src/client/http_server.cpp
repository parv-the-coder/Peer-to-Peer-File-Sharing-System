#include "client/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <sstream>

#include "common/socket_io.h"

namespace p2p {
namespace {

// Caps on what we will read from a client, so a malformed or hostile
// request cannot make us buffer without bound.
constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 1 * 1024 * 1024;

std::string status_text(int code) {
  switch (code) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 404: return "Not Found";
  case 413: return "Payload Too Large";
  default:  return "Error";
  }
}

// Reads until the end of the headers, returning what was read. Anything
// past the blank line is body bytes already in the buffer.
bool read_headers(int fd, std::string &out) {
  char c;
  while (out.size() < kMaxHeader) {
    ssize_t r = read(fd, &c, 1);
    if (r <= 0) return false;
    out.push_back(c);
    if (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) {
      return true;
    }
  }
  return false;
}

} // namespace

std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
    case '"':  out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n";  break;
    case '\r': out += "\\r";  break;
    case '\t': out += "\\t";  break;
    default:
      if (c < 0x20) {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  return out;
}

HttpServer::HttpServer() = default;
HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string &method, const std::string &path,
                       Handler h) {
  routes_[method + " " + path] = std::move(h);
}

bool HttpServer::start(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  // Loopback only. See the note in the header: this interface can run
  // commands, so it must not be reachable from the network.
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, 16) < 0) {
    close(fd);
    return false;
  }
  listen_fd_ = fd;
  port_ = port;
  thread_ = std::thread(&HttpServer::accept_loop, this);
  return true;
}

void HttpServer::accept_loop() {
  // Cached: stop() rewrites listen_fd_ after this thread is joined.
  const int listener = listen_fd_;
  while (true) {
    int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) {
      if (stopping_) return;
      continue;
    }
    if (stopping_) {
      close(fd);
      return;
    }
    // Requests are short and handled inline; the browser opens few
    // connections and each is answered immediately.
    serve(fd);
    close(fd);
  }
}

void HttpServer::serve(int fd) {
  std::string raw;
  if (!read_headers(fd, raw)) return;

  std::istringstream head(raw);
  std::string method, path, version;
  head >> method >> path >> version;
  if (method.empty() || path.empty()) return;

  size_t content_length = 0;
  std::string line;
  std::getline(head, line); // rest of the request line
  while (std::getline(head, line) && line != "\r") {
    if (line.size() > 15) {
      std::string name = line.substr(0, 15);
      for (auto &ch : name) ch = static_cast<char>(tolower(ch));
      if (name == "content-length:") {
        content_length = strtoul(line.c_str() + 15, nullptr, 10);
      }
    }
  }

  HttpResponse res;
  if (content_length > kMaxBody) {
    res.status = 413;
    res.body = "request body too large";
  } else {
    HttpRequest req;
    req.method = method;
    // Ignore any query string when matching a route.
    req.path = path.substr(0, path.find('?'));

    if (content_length > 0) {
      size_t already = raw.find("\r\n\r\n");
      already = (already == std::string::npos) ? 0 : raw.size() - (already + 4);
      req.body = raw.substr(raw.size() - already);
      while (req.body.size() < content_length) {
        char buf[4096];
        ssize_t r = read(fd, buf, std::min(sizeof(buf), content_length - req.body.size()));
        if (r <= 0) break;
        req.body.append(buf, static_cast<size_t>(r));
      }
    }

    auto it = routes_.find(req.method + " " + req.path);
    if (it == routes_.end()) {
      res.status = 404;
      res.body = "not found";
    } else {
      res = it->second(req);
    }
  }

  std::ostringstream out;
  out << "HTTP/1.1 " << res.status << " " << status_text(res.status) << "\r\n"
      << "Content-Type: " << res.content_type << "\r\n"
      << "Content-Length: " << res.body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "Connection: close\r\n\r\n"
      << res.body;
  const std::string s = out.str();
  send_all(fd, s.data(), s.size());
}

void HttpServer::stop() {
  if (stopping_.exchange(true)) return;
  if (listen_fd_ >= 0) {
    // shutdown wakes the blocked accept without closing the descriptor
    // out from under it; the close waits until the thread has joined.
    shutdown(listen_fd_, SHUT_RDWR);
  }
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
}

} // namespace p2p
