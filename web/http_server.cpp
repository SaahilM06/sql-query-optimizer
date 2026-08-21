#include "http_server.hpp"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../util/json.hpp"

namespace sql::web {

namespace {

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

// Reads from `fd` until the buffer contains the full header block
// ("\r\n\r\n"), then reads any remaining Content-Length body bytes.
// Returns false if the connection closed before a full request arrived.
bool read_request(int fd, HttpRequest& out) {
    std::string buf;
    char chunk[4096];
    size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
        if (buf.size() > (1u << 20)) return false; // 1MiB header guard -- this server only expects small requests
    }

    std::string header_block = buf.substr(0, header_end);
    std::string body_so_far = buf.substr(header_end + 4);

    std::istringstream header_stream(header_block);
    std::string request_line;
    std::getline(header_stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    std::istringstream request_line_stream(request_line);
    std::string full_path;
    request_line_stream >> out.method >> full_path;

    size_t q = full_path.find('?');
    if (q == std::string::npos) {
        out.path = full_path;
    } else {
        out.path = full_path.substr(0, q);
        out.query = full_path.substr(q + 1);
    }

    std::string header_line;
    while (std::getline(header_stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        if (header_line.empty()) continue;
        size_t colon = header_line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = to_lower(header_line.substr(0, colon));
        size_t value_start = header_line.find_first_not_of(' ', colon + 1);
        out.headers[key] = value_start == std::string::npos ? "" : header_line.substr(value_start);
    }

    size_t content_length = 0;
    if (auto it = out.headers.find("content-length"); it != out.headers.end()) {
        try {
            content_length = static_cast<size_t>(std::stoul(it->second));
        } catch (const std::exception&) {
            return false; // malformed Content-Length -- drop the connection rather than guess
        }
    }

    out.body = std::move(body_so_far);
    while (out.body.size() < content_length) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        out.body.append(chunk, static_cast<size_t>(n));
    }
    if (out.body.size() > content_length) out.body.resize(content_length);

    return true;
}

void write_response(int fd, const HttpResponse& resp) {
    std::ostringstream os;
    os << "HTTP/1.1 " << resp.status << " " << status_text(resp.status) << "\r\n";
    os << "Content-Type: " << resp.content_type << "\r\n";
    os << "Content-Length: " << resp.body.size() << "\r\n";
    os << "Connection: close\r\n";
    os << "\r\n";
    os << resp.body;
    std::string out = os.str();

    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

} // namespace

HttpServer::HttpServer(std::string host, int port) : host_(std::move(host)), port_(port) {}

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    routes_[method + " " + path] = std::move(handler);
}

void HttpServer::set_fallback(Handler handler) { fallback_ = std::move(handler); }

void HttpServer::handle_connection(int client_fd) {
    try {
        HttpRequest req;
        if (read_request(client_fd, req)) {
            auto it = routes_.find(req.method + " " + req.path);
            HttpResponse resp;
            try {
                if (it != routes_.end()) {
                    resp = it->second(req);
                } else if (fallback_) {
                    resp = fallback_(req);
                } else {
                    resp = HttpResponse::text("not found", 404);
                }
            } catch (const std::exception& e) {
                sql::util::JsonValue err = sql::util::JsonValue::make_object();
                err.object_val["error"] = sql::util::JsonValue::make_string(e.what());
                resp = HttpResponse::json(sql::util::to_json(err), 500);
            }
            write_response(client_fd, resp);
        }
    } catch (...) {
        // A malformed request or socket error mid-parse -- drop the
        // connection rather than let an exception escape a detached
        // thread, which would call std::terminate() and kill the server
        // over a single bad request.
    }
    ::close(client_fd);
}

void HttpServer::run() {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) throw std::runtime_error("web: failed to create socket");

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = (host_ == "0.0.0.0") ? INADDR_ANY : inet_addr(host_.c_str());

    if (::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd);
        throw std::runtime_error("web: failed to bind " + host_ + ":" + std::to_string(port_));
    }
    if (::listen(server_fd, 16) < 0) {
        ::close(server_fd);
        throw std::runtime_error("web: failed to listen on " + host_ + ":" + std::to_string(port_));
    }

    for (;;) {
        int client_fd = ::accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        std::thread(&HttpServer::handle_connection, this, client_fd).detach();
    }
}

} // namespace sql::web
