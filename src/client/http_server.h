#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace p2p {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain";
  std::string body;
};

// A deliberately small HTTP/1.1 server for the local web interface.
//
// Hand-rolled on the same sockets everything else uses rather than
// pulling in a framework: it needs to serve one page and two endpoints,
// and the socket handling was already here.
//
// It binds 127.0.0.1 only, never a public interface. The interface can
// run any command the logged-in user could run, so exposing it on the
// network would hand that ability to anyone who could reach the port --
// there is no separate authentication in front of it.
class HttpServer {
public:
  using Handler = std::function<HttpResponse(const HttpRequest &)>;

  HttpServer();
  ~HttpServer();
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  void route(const std::string &method, const std::string &path, Handler h);

  // Binds 127.0.0.1:port and starts serving. Returns false if the port is
  // unavailable.
  bool start(int port);
  void stop();
  int port() const { return port_; }

private:
  void accept_loop();
  void serve(int fd);

  int listen_fd_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::map<std::string, Handler> routes_; // "METHOD path" -> handler
};

// Escapes a string for embedding in a JSON document.
std::string json_escape(const std::string &s);

} // namespace p2p
