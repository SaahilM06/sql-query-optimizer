#include "http_client.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace sql::distributed {

namespace {

// Same non-blocking-connect-with-select-timeout pattern as
// integration/cache_client.cpp's CacheClient::connect() -- an unreachable
// worker should fail fast, not hang the coordinator.
int connect_with_timeout(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0 || result == nullptr) return -1;

    int fd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) break;
        if (errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = ::select(fd + 1, nullptr, &write_set, nullptr, &tv);
        if (sel <= 0) {
            ::close(fd);
            fd = -1;
            continue;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            ::close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    ::freeaddrinfo(result);
    if (fd < 0) return -1;

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::optional<HttpClientResponse> read_response(int fd) {
    std::string buf;
    char chunk[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return std::nullopt;
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
        if (buf.size() > (1u << 20)) return std::nullopt;
    }

    std::string header_block = buf.substr(0, header_end);
    std::string body_so_far = buf.substr(header_end + 4);

    std::istringstream header_stream(header_block);
    std::string status_line;
    std::getline(header_stream, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();

    int status = 0;
    {
        std::istringstream ss(status_line);
        std::string http_version;
        ss >> http_version >> status;
    }

    size_t content_length = 0;
    std::string header_line;
    while (std::getline(header_stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        auto colon = header_line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = header_line.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "content-length") {
            size_t value_start = header_line.find_first_not_of(' ', colon + 1);
            try {
                content_length = static_cast<size_t>(std::stoul(header_line.substr(value_start)));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    }

    HttpClientResponse resp;
    resp.status = status;
    resp.body = std::move(body_so_far);
    while (resp.body.size() < content_length) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return std::nullopt;
        resp.body.append(chunk, static_cast<size_t>(n));
    }
    if (resp.body.size() > content_length) resp.body.resize(content_length);

    return resp;
}

} // namespace

namespace {

// An artificial per-request delay, for demonstrating how conditions (not
// just data size) should shift the broadcast-vs-shuffle bandit's learned
// preference -- shuffle always makes more round trips than broadcast for
// the same join (gather both sides + redistribute vs. gather one side +
// replicate), so the gap between them should widen as this grows. Read
// fresh on every call rather than cached, so a coordinator session can
// have the knob turned up mid-run for a live before/after comparison. 0 or
// unset means no simulated delay (the default -- real localhost calls
// already have their own real, much smaller latency).
int simulated_latency_ms() {
    const char* env = std::getenv("SQLOPT_SIMULATED_LATENCY_MS");
    if (env == nullptr) return 0;
    try {
        return std::stoi(env);
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

std::optional<HttpClientResponse> http_post_json(const std::string& host, int port, const std::string& path,
                                                   const std::string& json_body, int timeout_ms) {
    int delay_ms = simulated_latency_ms();
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    int fd = connect_with_timeout(host, port, timeout_ms);
    if (fd < 0) return std::nullopt;

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Content-Type: application/json\r\n";
    req << "Content-Length: " << json_body.size() << "\r\n";
    req << "Connection: close\r\n";
    req << "\r\n";
    req << json_body;

    if (!send_all(fd, req.str())) {
        ::close(fd);
        return std::nullopt;
    }

    auto resp = read_response(fd);
    ::close(fd);
    return resp;
}

} // namespace sql::distributed
