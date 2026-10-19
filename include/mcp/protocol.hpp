#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mcp/dispatcher.hpp"
#include "mcp/http_transport.hpp"
#include "mcp/json.hpp"
#include "mcp/tool_registry.hpp"

namespace mcp {

struct ServerInfo {
    std::string name    = "mcp-cpp-server";
    std::string title   = "MCP C++ Server";
    std::string version = "0.1.0";
    std::string instructions;
};

struct ServerConfig {
    int                       port = 7474;
    std::string               endpoint = "/mcp";
    std::string               token;  // empty: no authentication
    std::chrono::milliseconds tool_timeout{10000};
    std::chrono::milliseconds frame_budget{8};

    // HTTP+SSE transport of protocol 2024-11-05 for clients that predate Streamable HTTP.
    bool        legacy_sse        = true;
    std::string sse_endpoint      = "/sse";
    std::string messages_endpoint = "/messages";
    int         max_sse_sessions  = 8;
};

// JSON-RPC 2.0 over Streamable HTTP, dual-era: the 2026-07-28 shape
// (per-request _meta version, server/discover, mirrored Mcp-* headers) and
// the initialize handshake of 2024-11-05 .. 2025-11-25, single messages or
// batches. Streamable requests get one JSON body, never an SSE stream; the
// legacy transport answers on the session's event stream instead.
class Protocol {
public:
    Protocol(const ToolRegistry& tools, Dispatcher& dispatcher, ServerInfo info,
             ServerConfig config);

    // Runs on a network thread.
    HttpResponse handle(const HttpRequest& req);

    // Ends every open event stream so the transport can stop.
    void shutdown();

    // Paths the transport has to route here.
    std::vector<std::string> paths() const;

    // Thread-safe sink for one-line diagnostics (also called from the main thread).
    std::function<void(std::string)> on_log;

private:
    struct SseSession;

    HttpResponse handle_streamable(const HttpRequest& http);
    HttpResponse open_sse(const HttpRequest& http);
    HttpResponse post_legacy(const HttpRequest& http);

    bool authorized(const HttpRequest& http) const;
    HttpResponse run_body(const HttpRequest& http, bool single_headers) const;
    HttpResponse run_message(const std::shared_ptr<json::Document>& owner, const json::Value& msg,
                             const HttpRequest& http, bool single) const;
    HttpResponse dispatch(const json::Value& msg, bool modern) const;
    HttpResponse call_tool(const std::shared_ptr<json::Document>& owner, const json::Value& msg) const;

    void close_session(const std::string& id);

    const ToolRegistry&       tools_;
    Dispatcher&               dispatcher_;
    ServerInfo                info_;
    ServerConfig              config_;

    std::mutex                                                   sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<SseSession>> sessions_;
    std::atomic<bool>                                            closing_{false};
};

}  // namespace mcp
