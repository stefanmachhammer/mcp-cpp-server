#include "mcp/server.hpp"

#include <utility>

namespace mcp {

McpServer::McpServer() = default;

McpServer::~McpServer() { stop(); }

bool McpServer::start(const ServerConfig& cfg, std::string* err) {
    if (running()) return true;
    config_ = cfg;
    dispatcher_.reopen();
    protocol_ = std::make_unique<Protocol>(tools_, dispatcher_, info_, cfg);
    protocol_->on_log = [this](std::string line) { queue_log(std::move(line), false); };

    Protocol* proto = protocol_.get();
    const bool ok = transport_.start(cfg.port, protocol_->paths(),
        [proto](const HttpRequest& req) -> HttpResponse {
            try {
                return proto->handle(req);
            } catch (const std::exception& e) {
                json::Document d(rapidjson::kObjectType);
                auto& a = d.GetAllocator();
                json::set(d, "jsonrpc", "2.0", a);
                json::set(d, "id", json::Value(rapidjson::kNullType), a);
                json::Value error = json::object();
                json::set(error, "code", -32603, a);
                json::set(error, "message", std::string_view(e.what()), a);
                json::set(d, "error", std::move(error), a);
                HttpResponse r;
                r.status = 500;
                r.body = json::dump(d);
                return r;
            } catch (...) {
                HttpResponse r;
                r.status = 500;
                r.body = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"internal error"}})";
                return r;
            }
        }, err);
    if (!ok) {
        protocol_.reset();
        return false;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(cfg.port);
    queue_log("listening on " + base + cfg.endpoint +
              (cfg.legacy_sse ? " (legacy SSE: " + base + cfg.sse_endpoint + ")" : std::string()) +
              (cfg.token.empty() ? ", no token" : ", bearer token required"), false);
    return true;
}

void McpServer::stop() {
    if (!running() && !protocol_) return;
    dispatcher_.shutdown();  // releases request threads blocked on futures
    if (protocol_) protocol_->shutdown();  // ends open event streams
    transport_.stop();
    protocol_.reset();
    queue_log("stopped", false);
}

void McpServer::pump() {
    dispatcher_.pump(config_.frame_budget);
    std::deque<std::pair<std::string, bool>> lines;
    {
        std::lock_guard lock(log_mutex_);
        lines.swap(log_lines_);
    }
    if (on_log)
        for (auto& [text, is_error] : lines) on_log(text, is_error);
}

void McpServer::queue_log(std::string line, bool is_error) {
    std::lock_guard lock(log_mutex_);
    log_lines_.emplace_back(std::move(line), is_error);
    if (log_lines_.size() > 256) log_lines_.pop_front();
}

}  // namespace mcp
