#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "mcp/dispatcher.hpp"
#include "mcp/http_transport.hpp"
#include "mcp/protocol.hpp"
#include "mcp/tool_registry.hpp"

namespace mcp {

struct ServerConfig {
    int                       port = 7474;
    std::string               endpoint = "/mcp";
    std::string               token;  // empty: no authentication
    std::chrono::milliseconds tool_timeout{10000};
    std::chrono::milliseconds frame_budget{8};
};

// Owns the registry, the main-thread queue and the listener. Tools are
// registered once before the first start(); pump() must run every frame.
class McpServer {
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&)            = delete;
    McpServer& operator=(const McpServer&) = delete;

    ToolRegistry& tools() noexcept { return tools_; }
    void set_info(ServerInfo info) { info_ = std::move(info); }

    bool start(const ServerConfig& cfg, std::string* err);
    void stop();
    void pump();

    bool running() const noexcept { return transport_.running(); }
    int  port()    const noexcept { return config_.port; }
    const ServerConfig& config() const noexcept { return config_; }

    // Main-thread callback for diagnostics gathered from any thread.
    std::function<void(std::string_view line, bool is_error)> on_log;

private:
    void queue_log(std::string line, bool is_error);

    ToolRegistry              tools_;
    Dispatcher                dispatcher_;
    ServerInfo                info_;
    ServerConfig              config_;
    std::unique_ptr<Protocol> protocol_;
    HttpTransport             transport_;

    std::mutex                                   log_mutex_;
    std::deque<std::pair<std::string, bool>>     log_lines_;
};

}  // namespace mcp
