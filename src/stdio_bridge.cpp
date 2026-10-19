#define _CRT_SECURE_NO_WARNINGS

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "mcp/stdio_bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "mcp/json.hpp"

namespace mcp {

namespace {

constexpr const char* kUsage =
    "options: [--url http://host:port/path] [--host H] [--port N] [--path P]\n"
    "         [--token T | --token-env VAR] [--no-token]\n";

bool parse_url(std::string_view url, BridgeOptions& opt) {
    constexpr std::string_view scheme = "http://";
    if (url.substr(0, scheme.size()) != scheme) return false;
    url.remove_prefix(scheme.size());
    const std::size_t slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    opt.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    if (const std::size_t colon = authority.rfind(':');
        colon != std::string_view::npos && authority.find(']', colon) == std::string_view::npos) {
        opt.port  = std::atoi(std::string(authority.substr(colon + 1)).c_str());
        authority = authority.substr(0, colon);
    } else {
        opt.port = 80;
    }
    if (authority.size() > 2 && authority.front() == '[' && authority.back() == ']')
        authority = authority.substr(1, authority.size() - 2);
    opt.host = std::string(authority);
    return !opt.host.empty() && opt.port > 0;
}

void write_line(std::string_view s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// A Streamable HTTP server may answer with an event stream; every data payload is one message.
void write_sse_messages(const std::string& body) {
    std::string data;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        std::size_t end = body.find('\n', pos);
        if (end == std::string::npos) end = body.size();
        std::string_view line(body.data() + pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) {
            if (!data.empty()) write_line(data);
            data.clear();
        } else if (line.substr(0, 5) == "data:") {
            line.remove_prefix(line.size() > 5 && line[5] == ' ' ? 6 : 5);
            if (!data.empty()) data += '\n';
            data += line;
        }
        pos = end + 1;
    }
    if (!data.empty()) write_line(data);
}

}  // namespace

bool parse_bridge_args(int argc, char** argv, BridgeOptions& opt, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--help" || a == "-h") return fail(kUsage);
        if (a == "--no-token") {
            opt.token.clear();
            continue;
        }
        if (!has_value) return fail("missing value for " + a + "\n" + kUsage);
        const std::string v = argv[++i];
        if (a == "--url") {
            if (!parse_url(v, opt)) return fail("bad --url (expected http://host:port/path): " + v);
        } else if (a == "--host") {
            opt.host = v;
        } else if (a == "--port") {
            opt.port = std::atoi(v.c_str());
        } else if (a == "--path") {
            opt.path = v;
        } else if (a == "--token") {
            opt.token = v;
        } else if (a == "--token-env") {
            const char* t = std::getenv(v.c_str());
            if (!t || !*t) return fail("environment variable " + v + " is not set");
            opt.token = t;
        } else {
            return fail("unknown option " + a + "\n" + kUsage);
        }
    }
    return true;
}

int run_stdio_bridge(const BridgeOptions& opt) {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    httplib::Client cli(opt.host, opt.port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(120, 0);
    cli.set_keep_alive(true);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        json::Document req;
        std::string method, name, version;
        const json::Value* id = nullptr;
        if (json::parse(line, req) && req.IsObject()) {
            method = std::string(json::get_string(req, "method"));
            id     = json::find(req, "id");
            if (const json::Value* p = json::find(req, "params"); p && p->IsObject()) {
                name = std::string(json::get_string(*p, "name"));
                if (const json::Value* m = json::find(*p, "_meta"); m && m->IsObject())
                    version = std::string(
                        json::get_string(*m, "io.modelcontextprotocol/protocolVersion"));
            }
        }

        httplib::Headers headers{{"Accept", "application/json, text/event-stream"}};
        if (!opt.token.empty()) headers.emplace("Authorization", "Bearer " + opt.token);
        if (!method.empty())  headers.emplace("Mcp-Method", method);
        if (!name.empty())    headers.emplace("Mcp-Name", name);
        if (!version.empty()) headers.emplace("MCP-Protocol-Version", version);

        httplib::Result res = cli.Post(opt.path, headers, line, "application/json");
        if (!res) {
            std::fprintf(stderr, "[%s] server not reachable at http://%s:%d%s (%s)\n",
                         opt.name.c_str(), opt.host.c_str(), opt.port, opt.path.c_str(),
                         httplib::to_string(res.error()).c_str());
            if (id) {
                json::Document out(rapidjson::kObjectType);
                json::Alloc& a = out.GetAllocator();
                json::Value idv;
                idv.CopyFrom(*id, a);
                json::Value err = json::object();
                json::set(err, "code", -32000, a);
                json::set(err, "message",
                          "MCP server is not reachable" +
                              (opt.unreachable_hint.empty() ? std::string()
                                                            : "; " + opt.unreachable_hint),
                          a);
                json::set(out, "jsonrpc", "2.0", a);
                json::set(out, "id", std::move(idv), a);
                json::set(out, "error", std::move(err), a);
                write_line(json::dump(out));
            }
            continue;
        }
        if (res->status == 202 || res->body.empty()) continue;  // notification accepted
        if (res->get_header_value("Content-Type").rfind("text/event-stream", 0) == 0)
            write_sse_messages(res->body);
        else
            write_line(res->body);
    }
    return 0;
}

}  // namespace mcp
