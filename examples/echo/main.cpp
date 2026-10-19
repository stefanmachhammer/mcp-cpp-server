#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "mcp/server.hpp"

namespace {
std::atomic<bool> g_quit{false};
}

int main(int argc, char** argv) {
    std::signal(SIGINT, [](int) { g_quit = true; });

    mcp::McpServer server;
    server.set_info({.name = "mcp-echo", .title = "MCP echo example", .version = "0.1.0"});
    server.on_log = [](std::string_view line, bool is_error) {
        std::fprintf(is_error ? stderr : stdout, "[mcp] %.*s\n", static_cast<int>(line.size()),
                     line.data());
    };

    server.tools().add({
        .name         = "echo",
        .title        = "Echo",
        .description  = "Returns the given text unchanged.",
        .input_schema = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})",
        .read_only    = true,
        .idempotent   = true,
        .handler      = [](const mcp::json::Value& args) -> std::optional<mcp::ToolResult> {
            return mcp::ToolResult::text(std::string(mcp::json::get_string(args, "text")));
        },
    });

    int frames = 0;
    server.tools().add({
        .name        = "frame_count",
        .title       = "Frame count",
        .description = "Number of main-loop iterations since start.",
        .read_only   = true,
        .handler     = [&frames](const mcp::json::Value&) -> std::optional<mcp::ToolResult> {
            mcp::json::Document d(rapidjson::kObjectType);
            mcp::json::set(d, "frames", frames, d.GetAllocator());
            return mcp::ToolResult::json(std::move(d));
        },
    });

    mcp::ServerConfig cfg;
    if (argc > 1) cfg.port = std::stoi(argv[1]);
    std::string err;
    if (!server.start(cfg, &err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }

    // Stand-in for a game or editor frame loop: tool handlers only ever run inside pump().
    while (!g_quit) {
        server.pump();
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    server.stop();
    server.pump();
    return 0;
}
