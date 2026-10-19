#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

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

// JSON-RPC 2.0 over Streamable HTTP, dual-era: the 2026-07-28 shape
// (per-request _meta version, server/discover, mirrored Mcp-* headers) and
// the initialize handshake of 2025-03-26 .. 2025-11-25. Stateless: every
// request is answered with a single JSON body, never an SSE stream.
class Protocol {
public:
    Protocol(const ToolRegistry& tools, Dispatcher& dispatcher, ServerInfo info,
             std::chrono::milliseconds tool_timeout);

    // Empty disables authentication; otherwise "Authorization: Bearer <token>" is required.
    void set_token(std::string token) { token_ = std::move(token); }

    // Runs on a network thread.
    HttpResponse handle(const HttpRequest& req) const;

    // Thread-safe sink for one-line diagnostics (also called from the main thread).
    std::function<void(std::string)> on_log;

private:
    HttpResponse dispatch(const json::Document& req, const HttpRequest& http,
                          std::string_view declared_version, bool modern) const;
    HttpResponse call_tool(std::shared_ptr<json::Document> req, bool modern) const;

    const ToolRegistry&       tools_;
    Dispatcher&               dispatcher_;
    ServerInfo                info_;
    std::chrono::milliseconds tool_timeout_;
    std::string               token_;
};

}  // namespace mcp
