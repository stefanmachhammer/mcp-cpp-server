#pragma once

#include <string>

namespace mcp {

// stdio <-> Streamable HTTP bridge for clients that only launch stdio servers
// (Claude Desktop, Codex CLI, Zed, ...). Every JSON-RPC line on stdin is POSTed
// to the server and each answer is written back as one line on stdout.
struct BridgeOptions {
    std::string host = "127.0.0.1";
    int         port = 7474;
    std::string path = "/mcp";
    std::string token;                      // empty: no Authorization header
    std::string name = "mcp-stdio-bridge";  // prefix for diagnostics on stderr
    std::string unreachable_hint;           // appended to the error sent when the server is down
};

// Applies --url, --host, --port, --path, --token, --token-env and --no-token on top of
// opt. Returns false with a message in err on a bad argument; --help sets err to the usage.
bool parse_bridge_args(int argc, char** argv, BridgeOptions& opt, std::string* err);

// Blocks until stdin closes; returns the process exit code.
int run_stdio_bridge(const BridgeOptions& opt);

}  // namespace mcp
