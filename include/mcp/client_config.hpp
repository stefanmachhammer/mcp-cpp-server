#pragma once

#include <span>
#include <string>
#include <vector>

namespace mcp {

enum class Client { ClaudeCode, ClaudeDesktop, Cursor, VSCode, Windsurf, GeminiCli, Codex };

struct ClientDescriptor {
    Client      id;
    const char* label;        // "Claude Code"
    const char* config_file;  // where the snippet goes, for display
    bool        uses_bridge;  // stdio-only client, launches the bridge
};

std::span<const ClientDescriptor> known_clients();

struct ClientConfigParams {
    std::string server_name = "mcp";
    std::string url = "http://127.0.0.1:7474/mcp";
    std::string token;  // empty: no Authorization header

    // Stdio-only clients start this program. Left empty, "mcp-stdio-bridge" is used
    // with --url/--token; with a command set, bridge_args are passed unchanged.
    std::string              bridge_command;
    std::vector<std::string> bridge_args;
};

// Ready-to-paste snippet in the client's own format (JSON, or TOML for Codex).
std::string client_config(Client client, const ClientConfigParams& params);

}  // namespace mcp
