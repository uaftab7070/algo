#include "preview/http_server.hpp"

#include "common/log.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ahpv {
namespace {

constexpr std::size_t kMaxHeader = 16 * 1024;
constexpr std::size_t kMaxBody = 64 * 1024;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string url_decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (text[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

bool send_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void write_response(int fd, const HttpResponse& r) {
    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + http_reason(r.status) + "\r\n";
    head += "Content-Type: " + r.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    head += "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n";
    head += "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' blob:; "
            "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n";
    for (const auto& [name, value] : r.headers) head += name + ": " + value + "\r\n";
    head += "\r\n";
    if (send_all(fd, head)) send_all(fd, r.body);
}

HttpResponse error_response(int status, const std::string& message) {
    HttpResponse r;
    r.status = status;
    r.body = "{\"error\":\"" + message + "\"}";
    return r;
}

}  // namespace

const char* http_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 421: return "Misdirected Request";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

std::string HttpRequest::header(const std::string& name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? std::string() : it->second;
}

std::string HttpRequest::query_param(const std::string& name) const {
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto end = query.find('&', start);
        const std::string pair = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto equals = pair.find('=');
        if (url_decode(pair.substr(0, equals)) == name) return equals == std::string::npos ? "" : url_decode(pair.substr(equals + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

HttpServer::HttpServer(std::string bind_address, int port, Handler handler)
    : bind_(std::move(bind_address)), port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    ::signal(SIGPIPE, SIG_IGN);
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (::inet_pton(AF_INET, bind_.c_str(), &address.sin_addr) != 1) throw std::runtime_error("invalid bind address " + bind_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(listen_fd_, 8) != 0) {
        const std::string error = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("cannot listen on " + bind_ + ":" + std::to_string(port_) + ": " + error);
    }
    if (port_ == 0) {
        socklen_t length = sizeof address;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
    }
    running_ = true;
    thread_ = std::thread([this] { serve(); });
    log_info("PRV-001", "preview listener started", json::Object{{"bind_address", bind_}, {"port", port_}});
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    log_info("PRV-002", "preview listener stopped");
}

void HttpServer::serve() {
    while (running_) {
        pollfd p{listen_fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, 200);
        if (r <= 0) continue;
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        timeval timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        try {
            handle_connection(fd);
        } catch (const std::exception& error) {
            log_error("PRV-090", std::string("request handling failed: ") + error.what());
            write_response(fd, error_response(500, "internal error"));
        }
        ::close(fd);
    }
}

void HttpServer::handle_connection(int fd) {
    std::string data;
    char buffer[4096];
    std::size_t header_end = std::string::npos;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > kMaxHeader) return write_response(fd, error_response(431, "headers too large"));
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<std::size_t>(n));
    }
    HttpRequest request;
    const std::string head = data.substr(0, header_end);
    std::size_t line_end = head.find("\r\n");
    const std::string request_line = head.substr(0, line_end);
    const auto s1 = request_line.find(' '), s2 = request_line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return write_response(fd, error_response(400, "malformed request line"));
    request.method = request_line.substr(0, s1);
    std::string target = request_line.substr(s1 + 1, s2 - s1 - 1);
    if (request_line.substr(s2 + 1).rfind("HTTP/1.", 0) != 0) return write_response(fd, error_response(400, "unsupported protocol"));
    const auto question = target.find('?');
    request.path = target.substr(0, question);
    if (question != std::string::npos) request.query = target.substr(question + 1);
    while (line_end != std::string::npos && line_end < head.size()) {
        const std::size_t next = head.find("\r\n", line_end + 2);
        const std::string line = head.substr(line_end + 2, next == std::string::npos ? std::string::npos : next - line_end - 2);
        line_end = next;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        request.headers[lower(line.substr(0, colon))] = value;
    }
    if (!request.header("transfer-encoding").empty()) return write_response(fd, error_response(501, "chunked bodies are not supported"));
    const std::string length_text = request.header("content-length");
    std::size_t length = 0;
    if (!length_text.empty()) {
        if (length_text.size() > 7 || length_text.find_first_not_of("0123456789") != std::string::npos)
            return write_response(fd, error_response(400, "invalid content-length"));
        length = std::stoul(length_text);
    } else if (request.method == "POST" || request.method == "PUT") {
        return write_response(fd, error_response(411, "content-length required"));
    }
    if (length > kMaxBody) return write_response(fd, error_response(413, "body too large"));
    request.body = data.substr(header_end + 4);
    while (request.body.size() < length) {
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, 0);
        if (n <= 0) return;
        request.body.append(buffer, static_cast<std::size_t>(n));
    }
    request.body.resize(length);
    write_response(fd, handler_(request));
}

}  // namespace ahpv
