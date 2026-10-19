// httplib pulls in winsock2 and must precede any windows.h include.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define CPPHTTPLIB_THREAD_POOL_COUNT 4
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "mcp/http_transport.hpp"

#include <utility>

namespace mcp {

struct HttpTransport::Impl {
    httplib::Server server;
    HttpHandler     handler;
};

HttpTransport::HttpTransport() : impl_(std::make_unique<Impl>()) {}

HttpTransport::~HttpTransport() { stop(); }

bool HttpTransport::start(int port, const std::string& endpoint, HttpHandler handler,
                          std::string* err) {
    if (running_) return true;
    impl_->handler = std::move(handler);
    httplib::Server& svr = impl_->server;

    auto relay = [this](const httplib::Request& req, httplib::Response& res) {
        HttpRequest hr;
        hr.method = req.method;
        hr.path   = req.path;
        hr.body   = req.body;
        hr.header = [&req](const char* name) { return req.get_header_value(name); };
        HttpResponse out = impl_->handler(hr);
        res.status = out.status;
        if (!out.allow.empty()) res.set_header("Allow", out.allow);
        if (!out.body.empty()) res.set_content(out.body, out.content_type);
    };
    svr.Post(endpoint, relay);
    svr.Get(endpoint, relay);
    svr.Delete(endpoint, relay);
    svr.Put(endpoint, relay);
    svr.Options(endpoint, relay);
    svr.set_keep_alive_max_count(64);
    svr.set_read_timeout(30, 0);
    svr.set_payload_max_length(16u * 1024u * 1024u);

    if (!svr.bind_to_port("127.0.0.1", port)) {
        if (err) *err = "could not bind 127.0.0.1:" + std::to_string(port);
        return false;
    }
    port_ = port;
    running_ = true;
    thread_ = std::thread([this] {
        impl_->server.listen_after_bind();
        running_ = false;
    });
    return true;
}

void HttpTransport::stop() {
    if (thread_.joinable()) {
        impl_->server.stop();
        thread_.join();
    }
    running_ = false;
    // A stopped httplib::Server cannot listen again; a fresh one replaces it.
    impl_ = std::make_unique<Impl>();
}

}  // namespace mcp
