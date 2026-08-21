#include "cache_client.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace sql::integration {

namespace {

// Same bound Go's server-side parser rejects above (proto-max-bulk-len,
// 512MiB) -- a corrupted or malicious reply shouldn't make this client try
// to allocate an unbounded buffer before validating anything.
constexpr size_t kMaxBulkLength = 512 * 1024 * 1024;

// Buffers reads from a live socket fd -- the live-connection counterpart to
// StringByteReader. Constructed fresh per request/reply; safe because this
// client is strictly synchronous (one in-flight request at a time), so the
// server never has more than the current reply's bytes in flight when this
// is reading.
class SocketByteReader : public ByteReader {
public:
    explicit SocketByteReader(int fd) : fd_(fd) {}

    std::string read_line() override {
        std::string line;
        for (;;) {
            if (pos_ >= buf_.size()) fill();
            char c = buf_[pos_++];
            line.push_back(c);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
                line.erase(line.size() - 2);
                return line;
            }
        }
    }

    std::string read_exact(size_t n) override {
        std::string out;
        out.reserve(n);
        while (out.size() < n) {
            if (pos_ >= buf_.size()) fill();
            size_t take = std::min(n - out.size(), buf_.size() - pos_);
            out.append(buf_, pos_, take);
            pos_ += take;
        }
        return out;
    }

private:
    int fd_;
    std::string buf_;
    size_t pos_ = 0;

    void fill() {
        char tmp[4096];
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (n <= 0) throw std::runtime_error("cache_client: connection closed or recv failed");
        buf_.assign(tmp, tmp + n);
        pos_ = 0;
    }
};

} // namespace

std::string encode_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) {
        out += "$" + std::to_string(a.size()) + "\r\n";
        out += a;
        out += "\r\n";
    }
    return out;
}

std::string StringByteReader::read_line() {
    size_t nl = data_.find('\n', pos_);
    if (nl == std::string::npos) throw std::runtime_error("resp: unexpected EOF reading line");
    std::string line = data_.substr(pos_, nl - pos_);
    pos_ = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::string StringByteReader::read_exact(size_t n) {
    if (pos_ + n > data_.size()) throw std::runtime_error("resp: unexpected EOF reading exact bytes");
    std::string out = data_.substr(pos_, n);
    pos_ += n;
    return out;
}

Reply parse_reply(ByteReader& reader) {
    std::string line = reader.read_line();
    if (line.empty()) throw std::runtime_error("resp: empty reply line");

    char prefix = line[0];
    std::string rest = line.substr(1);
    Reply r;

    switch (prefix) {
        case '+':
            r.type = Reply::Type::Ok;
            r.string_val = rest;
            return r;
        case '-':
            r.type = Reply::Type::Error;
            r.string_val = rest;
            return r;
        case ':':
            r.type = Reply::Type::Integer;
            r.integer_val = std::stoll(rest);
            return r;
        case '$': {
            int n = std::stoi(rest);
            if (n == -1) {
                r.type = Reply::Type::Null;
                return r;
            }
            if (n < 0) throw std::runtime_error("resp: negative bulk length");
            if (static_cast<size_t>(n) > kMaxBulkLength) {
                throw std::runtime_error("resp: bulk length exceeds max");
            }
            std::string payload = reader.read_exact(static_cast<size_t>(n) + 2); // + trailing \r\n
            r.type = Reply::Type::Bulk;
            r.bulk_val = payload.substr(0, static_cast<size_t>(n));
            return r;
        }
        default:
            throw std::runtime_error(std::string("resp: unknown reply type byte '") + prefix + "'");
    }
}

CacheClient::CacheClient(std::string host, int port, int connect_timeout_ms)
    : host_(std::move(host)), port_(port), connect_timeout_ms_(connect_timeout_ms) {}

CacheClient::~CacheClient() { close_connection(); }

void CacheClient::close_connection() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool CacheClient::connect() {
    close_connection();

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port_);
    if (::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0 || result == nullptr) {
        return false;
    }

    int fd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) break; // connected immediately (e.g. localhost)

        if (errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }

        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval tv;
        tv.tv_sec = connect_timeout_ms_ / 1000;
        tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;

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
        break; // connected within the timeout
    }
    ::freeaddrinfo(result);

    if (fd < 0) return false;

    // Blocking mode from here on: this client issues one request and waits
    // for its reply at a time, so ordinary blocking recv/send is sufficient
    // once the (potentially slow) connect handshake is past.
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    fd_ = fd;
    return true;
}

std::optional<Reply> CacheClient::send_command(const std::vector<std::string>& args) {
    if (fd_ < 0) return std::nullopt;

    std::string encoded = encode_command(args);
    size_t sent = 0;
    while (sent < encoded.size()) {
        ssize_t n = ::send(fd_, encoded.data() + sent, encoded.size() - sent, 0);
        if (n <= 0) {
            close_connection();
            return std::nullopt;
        }
        sent += static_cast<size_t>(n);
    }

    try {
        SocketByteReader reader(fd_);
        return parse_reply(reader);
    } catch (const std::exception&) {
        close_connection();
        return std::nullopt;
    }
}

std::optional<std::string> CacheClient::get(const std::string& key) {
    auto reply = send_command({"GET", key});
    if (!reply.has_value() || reply->type != Reply::Type::Bulk) return std::nullopt;
    return reply->bulk_val;
}

bool CacheClient::set(const std::string& key, const std::string& value, int ttl_seconds) {
    std::vector<std::string> args = {"SET", key, value};
    if (ttl_seconds > 0) {
        args.push_back("EX");
        args.push_back(std::to_string(ttl_seconds));
    }
    auto reply = send_command(args);
    return reply.has_value() && reply->type == Reply::Type::Ok;
}

std::optional<Reply> CacheClient::command(const std::vector<std::string>& args) { return send_command(args); }

} // namespace sql::integration
