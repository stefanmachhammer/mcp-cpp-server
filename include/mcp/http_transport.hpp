#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace mcp {

// Transport-neutral view of one HTTP exchange so the protocol layer never
// sees the HTTP library.
struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::function<std::string(const char* name)> header;  // "" when absent
};

struct HttpResponse {
    int         status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::string allow;  // Allow header for 405
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// Localhost-only HTTP/1.1 listener on its own thread. The handler runs on the
// listener's worker threads and must never touch host state directly.
class HttpTransport {
public:
    HttpTransport();
    ~HttpTransport();

    HttpTransport(const HttpTransport&)            = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;

    bool start(int port, const std::string& endpoint, HttpHandler handler, std::string* err);
    void stop();

    bool running() const noexcept { return running_.load(); }
    int  port()    const noexcept { return port_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
    int                   port_ = 0;
};

}  // namespace mcp
