#pragma once

#include <optional>
#include <string>

namespace sql::distributed {

struct HttpClientResponse {
    int status = 0;
    std::string body;
};

// A minimal blocking HTTP/1.1 client: one request, one response, then
// closes the connection (matches web/http_server.hpp's server-side
// "Connection: close" -- no keep-alive on either end). Returns
// std::nullopt on any connection/timeout/malformed-response failure rather
// than throwing, so a dead worker is reported as "unreachable" instead of
// crashing the coordinator mid-query.
std::optional<HttpClientResponse> http_post_json(const std::string& host, int port, const std::string& path,
                                                   const std::string& json_body, int timeout_ms = 5000);

} // namespace sql::distributed
