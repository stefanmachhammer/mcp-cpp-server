#include "mcp/client_config.hpp"

#include <array>

#include <rapidjson/prettywriter.h>

#include "mcp/json.hpp"

namespace mcp {

namespace {

constexpr std::array<ClientDescriptor, 7> kClients = {{
    {Client::ClaudeCode,    "Claude Code",    ".mcp.json",                            false},
    {Client::ClaudeDesktop, "Claude Desktop", "claude_desktop_config.json",           true},
    {Client::Cursor,        "Cursor",         ".cursor/mcp.json",                     false},
    {Client::VSCode,        "VS Code",        ".vscode/mcp.json",                     false},
    {Client::Windsurf,      "Windsurf",       "~/.codeium/windsurf/mcp_config.json",  false},
    {Client::GeminiCli,     "Gemini CLI",     ".gemini/settings.json",                false},
    {Client::Codex,         "Codex CLI",      "~/.codex/config.toml",                 true},
}};

std::string pretty(const json::Value& v) {
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(sb);
    w.SetIndent(' ', 2);
    v.Accept(w);
    return std::string(sb.GetString(), sb.GetSize()) + "\n";
}

std::string toml_string(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

bool bare_toml_key(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    return true;
}

void bridge_command(const ClientConfigParams& p, std::string& command,
                    std::vector<std::string>& args) {
    if (!p.bridge_command.empty()) {
        command = p.bridge_command;
        args    = p.bridge_args;
        return;
    }
    command = "mcp-stdio-bridge";
    args    = {"--url", p.url};
    if (!p.token.empty()) {
        args.push_back("--token");
        args.push_back(p.token);
    }
}

}  // namespace

std::span<const ClientDescriptor> known_clients() { return kClients; }

std::string client_config(Client client, const ClientConfigParams& p) {
    if (client == Client::Codex) {
        std::string command;
        std::vector<std::string> args;
        bridge_command(p, command, args);
        std::string s = "[mcp_servers." +
                        (bare_toml_key(p.server_name) ? p.server_name : toml_string(p.server_name)) +
                        "]\ncommand = " + toml_string(command) + "\nargs = [";
        for (std::size_t i = 0; i < args.size(); ++i)
            s += (i ? ", " : "") + toml_string(args[i]);
        return s + "]\n";
    }

    json::Document doc(rapidjson::kObjectType);
    json::Alloc& a = doc.GetAllocator();
    json::Value server = json::object();

    if (client == Client::ClaudeDesktop) {
        std::string command;
        std::vector<std::string> args;
        bridge_command(p, command, args);
        json::set(server, "command", command, a);
        json::Value arr = json::array();
        for (const std::string& arg : args) arr.PushBack(json::str(arg, a), a);
        json::set(server, "args", std::move(arr), a);
    } else {
        const char* url_key = "url";
        if (client == Client::Windsurf) url_key = "serverUrl";
        if (client == Client::GeminiCli) url_key = "httpUrl";
        if (client == Client::ClaudeCode || client == Client::VSCode)
            json::set(server, "type", "http", a);
        json::set(server, url_key, p.url, a);
        if (!p.token.empty()) {
            json::Value headers = json::object();
            json::set(headers, "Authorization", "Bearer " + p.token, a);
            json::set(server, "headers", std::move(headers), a);
        }
    }

    json::Value servers = json::object();
    json::set(servers, p.server_name.c_str(), std::move(server), a);
    json::set(doc, client == Client::VSCode ? "servers" : "mcpServers", std::move(servers), a);
    return pretty(doc);
}

}  // namespace mcp
