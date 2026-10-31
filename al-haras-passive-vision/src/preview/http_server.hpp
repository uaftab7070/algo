#pragma once
// Minimal HTTP/1.1 server for the local preview (one request per connection,
// bounded header/body sizes, socket timeouts). It binds exactly the configured
// address; the configuration layer refuses non-loopback addresses by default.
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ahpv {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;  // Lower-case names.
    std::string body;
    std::string header(const std::string& name) const;
    std::string query_param(const std::string& name) const;
};

struct HttpResponse {
    int status{200};
    std::string content_type{"application/json"};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    HttpServer(std::string bind_address, int port, Handler handler);
    ~HttpServer();
    void start();  // Throws std::runtime_error when the address cannot be bound.
    void stop();
    int port() const { return port_; }

private:
    std::string bind_;
    int port_;
    Handler handler_;
    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    void serve();
    void handle_connection(int fd);
};

const char* http_reason(int status);

}  // namespace ahpv
