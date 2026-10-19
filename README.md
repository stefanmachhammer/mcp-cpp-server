# mcp-cpp-server

A small, embeddable [Model Context Protocol](https://modelcontextprotocol.io) server for C++20
applications with a main loop: games, editors, simulations, tools. AI clients (Claude Code,
Claude Desktop, Cursor, VS Code, Windsurf, Gemini CLI, Codex CLI, MCP Inspector, …) connect
and call tools that you register. The tools run on **your** main thread, between frames, so
they can touch application state without locks.

* Localhost only (`127.0.0.1`), optional bearer token, foreign `Origin` rejected.
* Stateless Streamable HTTP: one JSON body per request, no sessions.
* Serves the 2026-07-28 protocol shape (`server/discover`, per-request `_meta` version,
  `Mcp-Method` / `Mcp-Name` headers) and the `initialize` handshake of 2024-11-05 … 2025-11-25,
  single messages and JSON-RPC batches.
* The HTTP+SSE transport of 2024-11-05 (`GET /sse`, `POST /messages`) for older clients.
* A stdio bridge (`mcp-stdio-bridge`) for clients that only launch local processes.
* `mcp::client_config()` writes the config snippet for each supported client.
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
./build/mcp-echo 7474 secret   # same, with a bearer token
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
* Names must match `[A-Za-z0-9_-]{1,64}` and the schema must be `"type":"object"`; OpenAI and
  Gemini models reject anything else, so `add()` throws early instead.

### Threading

`HttpTransport` runs cpp-httplib on its own thread. The protocol layer parses and validates
there; a `tools/call` is queued in the `Dispatcher` and executed by `McpServer::pump()`. The
network thread waits for the result up to `ServerConfig::tool_timeout` and answers `-32000`
when the host is busy. `on_log` is also delivered from `pump()`, on the main thread.

### Responses

`GET`/`DELETE` on the Streamable endpoint answer 405, a foreign `Origin` 403, a missing or
wrong token 401, header/body mismatches 400 with `-32020`, unsupported protocol versions 400
with `-32022`. `resources/list`, `resources/templates/list`, `prompts/list` and
`logging/setLevel` get empty answers because several clients call them regardless of the
advertised capabilities.

### Legacy HTTP+SSE

On by default (`ServerConfig::legacy_sse`). `GET /sse` opens an event stream whose first event
is `endpoint` with `/messages?sessionId=…`; requests POSTed there are answered with 202 and the
JSON-RPC response arrives on the stream. Each open stream holds one HTTP worker, so at most
`max_sse_sessions` (8) are accepted. The same bearer token applies.

## Connecting a client

`mcp::client_config(client, params)` returns the snippet below for each client, so a host can
offer "copy config for …". The hand-written versions:

| Client | File | Transport |
| --- | --- | --- |
| Claude Code | `.mcp.json` | Streamable HTTP |
| Cursor | `.cursor/mcp.json` | Streamable HTTP |
| VS Code (Copilot) | `.vscode/mcp.json` | Streamable HTTP |
| Windsurf | `~/.codeium/windsurf/mcp_config.json` | Streamable HTTP |
| Gemini CLI | `.gemini/settings.json` | Streamable HTTP |
| Claude Desktop | `claude_desktop_config.json` | stdio bridge |
| Codex CLI | `~/.codex/config.toml` | stdio bridge |
| Older SSE-only clients | – | `http://127.0.0.1:7474/sse` |

**Claude Code**

```sh
claude mcp add --transport http my-game http://127.0.0.1:7474/mcp \
    --header "Authorization: Bearer secret"
```

**Cursor** (`mcpServers`, key `url`), **Windsurf** (key `serverUrl`), **Gemini CLI** (key
`httpUrl`) and **VS Code** (top-level `servers`, `"type": "http"`) differ only in those keys:

```json
{
  "mcpServers": {
    "my-game": {
      "url": "http://127.0.0.1:7474/mcp",
      "headers": { "Authorization": "Bearer secret" }
    }
  }
}
```

**Claude Desktop** and **Codex CLI** start the bridge, which forwards stdin/stdout to the server:

```json
{ "mcpServers": { "my-game": { "command": "C:/tools/mcp-stdio-bridge.exe",
                               "args": ["--url", "http://127.0.0.1:7474/mcp", "--token-env", "MY_GAME_TOKEN"] } } }
```

```toml
[mcp_servers.my-game]
command = "C:/tools/mcp-stdio-bridge.exe"
args = ["--url", "http://127.0.0.1:7474/mcp", "--token-env", "MY_GAME_TOKEN"]
```

`--token-env` reads the token from an environment variable so it stays out of the config file;
`--token` takes it literally. A host can also ship its own bridge executable around
`mcp::run_stdio_bridge()` that reads port and token from its settings.

**MCP Inspector**

```sh
npx @modelcontextprotocol/inspector --cli http://127.0.0.1:7474/mcp --transport http \
    --header "Authorization: Bearer secret" --method tools/list
```

## License

MIT, see [LICENSE](LICENSE).
