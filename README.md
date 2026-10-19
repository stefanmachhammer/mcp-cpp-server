# mcp-cpp-server

A small, embeddable [Model Context Protocol](https://modelcontextprotocol.io) server for C++20
applications with a main loop: games, editors, simulations, tools. An AI client (Claude Code,
MCP Inspector, …) connects over Streamable HTTP and calls tools that you register. The tools
run on **your** main thread, between frames, so they can touch application state without locks.

* Localhost only (`127.0.0.1`), optional bearer token, foreign `Origin` rejected.
* Stateless Streamable HTTP: one JSON body per request, no SSE, no sessions.
* Serves both the 2026-07-28 protocol shape (`server/discover`, per-request `_meta` version,
  `Mcp-Method` / `Mcp-Name` headers) and the `initialize` handshake of 2025-03-26 … 2025-11-25.
* A tool can span frames: return `std::nullopt` and it is called again next frame (useful for
  GPU readbacks and other deferred work).
* The main loop never blocks on the network; a slow tool makes the client time out, not the app.
* Dependencies: [RapidJSON](https://github.com/Tencent/rapidjson) and
  [cpp-httplib](https://github.com/yhirose/cpp-httplib), both header-only.

## Build

```sh
cmake -B build
cmake --build build
./build/mcp-echo            # listens on http://127.0.0.1:7474/mcp
```

CMake fetches both dependencies unless targets named `RapidJSON::RapidJSON` and
`httplib::httplib` already exist. To embed it:

```cmake
add_subdirectory(mcp-cpp-server)   # or FetchContent
target_link_libraries(my_app PRIVATE mcp::server)
```

Without CMake, compile `src/*.cpp` with `include/` plus the RapidJSON and cpp-httplib include
directories on the path, define `RAPIDJSON_HAS_STDSTRING=1`, and link `ws2_32` on Windows.

## Usage

```cpp
#include "mcp/server.hpp"

mcp::McpServer server;
server.set_info({.name = "my-game", .title = "My Game", .version = "1.0.0"});

server.tools().add({
    .name         = "player_get_position",
    .description  = "Current player position.",
    .read_only    = true,
    .handler      = [&](const mcp::json::Value&) -> std::optional<mcp::ToolResult> {
        mcp::json::Document d(rapidjson::kObjectType);
        auto& a = d.GetAllocator();
        mcp::json::set(d, "position", mcp::json::vec3(player.x, player.y, player.z, a), a);
        return mcp::ToolResult::json(std::move(d));
    },
});

mcp::ServerConfig cfg;
cfg.token = "secret";          // empty: no authentication
std::string err;
server.start(cfg, &err);

while (running) {
    server.pump();             // runs queued tool calls within cfg.frame_budget
    update();
    render();
}
```

### Tools

A `ToolSpec` holds the name, title, description, a JSON schema for the arguments
(`input_schema`), the annotations `read_only`, `destructive` and `idempotent`, and the handler.

* Read arguments with `json::get_int / get_number / get_string / get_bool / read_vec3`. The
  registry checks top-level `required` fields and types against the schema before the handler
  runs.
* Return `ToolResult::json(doc)` (text plus `structuredContent`), `ToolResult::text(...)`,
  `ToolResult::error(...)` (sets `isError`) or `ToolResult::image_png(bytes, len)`.
* Throwing from a handler is safe: it becomes an `isError` result.
* Tools are registered once before the first `start()`.

### Threading

`HttpTransport` runs cpp-httplib on its own thread. The protocol layer parses and validates
there; a `tools/call` is queued in the `Dispatcher` and executed by `McpServer::pump()`. The
network thread waits for the result up to `ServerConfig::tool_timeout` and answers `-32000`
when the host is busy. `on_log` is also delivered from `pump()`, on the main thread.

### Responses

`GET`/`DELETE` answer 405, a foreign `Origin` 403, a missing or wrong token 401,
header/body mismatches 400 with `-32020`, unsupported protocol versions 400 with `-32022`.

## Connecting a client

**Claude Code**

```sh
claude mcp add --transport http my-game http://127.0.0.1:7474/mcp \
    --header "Authorization: Bearer secret"
```

**MCP Inspector**

```sh
npx @modelcontextprotocol/inspector --cli http://127.0.0.1:7474/mcp --transport http \
    --header "Authorization: Bearer secret" --method tools/list
```

## License

MIT, see [LICENSE](LICENSE).
