#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sql::integration {

// ── RESP wire format (client side only) ──────────────────────────────────────
//
// Mirrors cache/internal/resp/{parser,encoder}.go, but only what a client
// needs: encode a command array to send, parse the non-array reply types the
// cache server actually sends back (this process never receives a RESP
// array in reply).

// Encodes `args` as a RESP array of bulk strings -- the format every client
// request uses. Pure string in/out, no I/O.
std::string encode_command(const std::vector<std::string>& args);

// A reply parser needs to read "a line" and "N exact bytes" from wherever
// the response bytes are coming from. Abstracting that (rather than parsing
// straight off a socket) is what makes parse_reply testable without a live
// connection -- the same seam Go's own resp package gets for free from
// bufio.Reader vs. strings.Reader.
class ByteReader {
public:
    virtual ~ByteReader() = default;
    // Reads up to and including the next "\r\n", returning the line with the
    // terminator stripped. Throws std::runtime_error on EOF/error.
    virtual std::string read_line() = 0;
    // Reads exactly n bytes. Throws std::runtime_error on EOF/error.
    virtual std::string read_exact(size_t n) = 0;
};

// A ByteReader over an in-memory buffer -- used by tests, and could equally
// serve a caller that already has the full response buffered.
class StringByteReader : public ByteReader {
public:
    explicit StringByteReader(std::string data) : data_(std::move(data)) {}
    std::string read_line() override;
    std::string read_exact(size_t n) override;

private:
    std::string data_;
    size_t pos_ = 0;
};

struct Reply {
    enum class Type { Ok, Error, Integer, Bulk, Null };

    Type type = Type::Null;
    std::string string_val; // Type::Ok (simple string) / Type::Error (message)
    int64_t integer_val = 0; // Type::Integer
    std::string bulk_val;    // Type::Bulk

    bool is_null() const { return type == Type::Null; }
};

// Parses exactly one non-array RESP reply ('+', '-', ':', '$') off `reader`.
Reply parse_reply(ByteReader& reader);

// ── TCP client ────────────────────────────────────────────────────────────────
//
// A thin, optimistic RESP client for the plan cache. The cache is treated as
// disposable infrastructure: any failure to connect or communicate makes
// get()/set() report "no cache entry" rather than throwing, so a
// down/unreachable cache never stops the optimizer from working -- it just
// stops the optimizer from being able to skip re-planning.
class CacheClient {
public:
    CacheClient(std::string host, int port, int connect_timeout_ms = 500);
    ~CacheClient();

    CacheClient(const CacheClient&) = delete;
    CacheClient& operator=(const CacheClient&) = delete;

    // Attempts to open (and keep open) one TCP connection to the cache,
    // bounded by connect_timeout_ms so an unreachable host fails fast
    // instead of hanging. Returns is_connected().
    bool connect();
    bool is_connected() const { return fd_ >= 0; }

    // std::nullopt on a miss, a communication failure, or no connection.
    std::optional<std::string> get(const std::string& key);

    // ttl_seconds == 0 means no expiry. Returns false on any failure
    // (including no connection) -- never throws.
    bool set(const std::string& key, const std::string& value, int ttl_seconds = 0);

    // Sends an arbitrary command (e.g. {"INFO"}, {"DBSIZE"}) and returns its
    // reply, or std::nullopt on any failure including no connection. get()/
    // set() are the common cases above this; this is the escape hatch for
    // callers that need something else (e.g. a CLI's "SHOW CACHE").
    std::optional<Reply> command(const std::vector<std::string>& args);

private:
    std::string host_;
    int port_;
    int connect_timeout_ms_;
    int fd_ = -1;

    // Sends one command and returns its reply, or std::nullopt on any I/O
    // failure (closing the connection so subsequent calls report "not
    // connected" rather than retrying a broken socket).
    std::optional<Reply> send_command(const std::vector<std::string>& args);
    void close_connection();
};

} // namespace sql::integration
