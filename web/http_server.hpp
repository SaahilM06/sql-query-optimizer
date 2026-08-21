#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace sql::web {

struct HttpRequest {
    std::string method;
    std::string path;  // path only, query string stripped
    std::string query; // raw query string, if any (unused by the JSON POST routes, kept for completeness)
    std::unordered_map<std::string, std::string> headers; // lowercase keys
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain";
    std::string body;

    static HttpResponse json(std::string body, int status = 200) {
        HttpResponse r;
        r.status = status;
        r.content_type = "application/json";
        r.body = std::move(body);
        return r;
    }
    static HttpResponse text(std::string body, int status = 200, std::string content_type = "text/plain") {
        HttpResponse r;
        r.status = status;
        r.content_type = std::move(content_type);
        r.body = std::move(body);
        return r;
    }
};

using Handler = std::function<HttpResponse(const HttpRequest&)>;

// A minimal single-purpose HTTP/1.1 server: one detached thread per
// connection, "Connection: close" on every response (no keep-alive), no
// chunked transfer encoding -- enough to serve a handful of JSON API
// routes and a small static frontend to a browser, not a general-purpose
// web server. No thread pool or connection cap; fine for a single-user dev
// tool, a real concern if this were ever exposed beyond localhost.
class HttpServer {
public:
    HttpServer(std::string host, int port);

    // Routes are matched by exact (method, path). Registering a route
    // after run() has started is not thread-safe -- register everything
    // first, then call run() once.
    void route(const std::string& method, const std::string& path, Handler handler);
    // Handles anything no exact route matched -- used here to serve static
    // files out of web/frontend/.
    void set_fallback(Handler handler);

    // Blocks, accepting connections until the process is killed. Throws
    // std::runtime_error if the socket can't be bound/listened on.
    void run();

private:
    std::string host_;
    int port_;
    std::unordered_map<std::string, Handler> routes_; // key: "METHOD path"
    Handler fallback_;

    void handle_connection(int client_fd);
};

} // namespace sql::web
