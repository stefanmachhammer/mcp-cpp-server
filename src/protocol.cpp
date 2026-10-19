#include "mcp/protocol.hpp"

#include <array>
#include <memory>
#include <utility>

#include "mcp/base64.hpp"

namespace mcp {

namespace {

constexpr std::array<std::string_view, 4> kSupportedVersions = {
    "2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"};
constexpr std::string_view kModernSince  = "2026-07-28";
constexpr std::string_view kLegacyLatest = "2025-11-25";
constexpr std::string_view kMetaVersion  = "io.modelcontextprotocol/protocolVersion";

// JSON-RPC and MCP error codes.
constexpr int kParseError       = -32700;
constexpr int kInvalidRequest   = -32600;
constexpr int kMethodNotFound   = -32601;
constexpr int kInvalidParams    = -32602;
constexpr int kInternalError    = -32603;
constexpr int kServerBusy       = -32000;
constexpr int kHeaderMismatch   = -32020;
constexpr int kUnsupportedVersion = -32022;

bool supported(std::string_view v) {
    for (std::string_view s : kSupportedVersions)
        if (s == v) return true;
    return false;
}

json::Value copy_id(const json::Value* id, json::Alloc& a) {
    json::Value v;
    if (id) v.CopyFrom(*id, a);
    return v;  // null when the request carried no id
}

std::string response_body(const json::Value* id, json::Value result, json::Document& doc) {
    json::Alloc& a = doc.GetAllocator();
    doc.SetObject();
    json::set(doc, "jsonrpc", "2.0", a);
    json::set(doc, "id", copy_id(id, a), a);
    json::set(doc, "result", std::move(result), a);
    return json::dump(doc);
}

HttpResponse error_response(int status, const json::Value* id, int code, std::string_view msg,
                            const json::Value* data = nullptr) {
    json::Document doc(rapidjson::kObjectType);
    json::Alloc& a = doc.GetAllocator();
    json::set(doc, "jsonrpc", "2.0", a);
    json::set(doc, "id", copy_id(id, a), a);
    json::Value err = json::object();
    json::set(err, "code", code, a);
    json::set(err, "message", msg, a);
    if (data) {
        json::Value d;
        d.CopyFrom(*data, a);
        json::set(err, "data", std::move(d), a);
    }
    json::set(doc, "error", std::move(err), a);
    HttpResponse r;
    r.status = status;
    r.body   = json::dump(doc);
    return r;
}

bool origin_allowed(std::string_view origin) {
    if (origin.empty()) return true;
    for (std::string_view ok : {"http://127.0.0.1", "http://localhost", "https://127.0.0.1",
                                "https://localhost", "http://[::1]", "https://[::1]"}) {
        if (origin.size() < ok.size() || origin.substr(0, ok.size()) != ok) continue;
        // Exact host, optionally followed by a port.
        if (origin.size() == ok.size() || origin[ok.size()] == ':') return true;
    }
    return false;
}

}  // namespace

Protocol::Protocol(const ToolRegistry& tools, Dispatcher& dispatcher, ServerInfo info,
                   std::chrono::milliseconds tool_timeout)
    : tools_(tools), dispatcher_(dispatcher), info_(std::move(info)),
      tool_timeout_(tool_timeout) {}

HttpResponse Protocol::handle(const HttpRequest& http) const {
    if (!origin_allowed(http.header("Origin")))
        return error_response(403, nullptr, kInvalidRequest, "Origin not allowed");

    if (http.method != "POST") {
        HttpResponse r;
        r.status = 405;
        r.allow  = "POST";
        r.body   = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Use POST"}})";
        return r;
    }

    if (!token_.empty()) {
        const std::string auth = http.header("Authorization");
        const std::string want = "Bearer " + token_;
        if (auth != want)
            return error_response(401, nullptr, kInvalidRequest,
                                  "Missing or invalid bearer token");
    }

    auto req = std::make_shared<json::Document>();
    std::string perr;
    if (!json::parse(http.body, *req, &perr))
        return error_response(400, nullptr, kParseError, "Parse error: " + perr);
    if (!req->IsObject() || json::get_string(*req, "jsonrpc") != "2.0" ||
        !json::has(*req, "method") || !json::find(*req, "method")->IsString())
        return error_response(400, nullptr, kInvalidRequest,
                              "Body must be a single JSON-RPC 2.0 request or notification");

    const std::string method(json::get_string(*req, "method"));
    const json::Value* id     = json::find(*req, "id");
    const json::Value* params = json::find(*req, "params");

    // Version: header and _meta must agree; either alone is accepted.
    const std::string header_version = http.header("MCP-Protocol-Version");
    std::string meta_version;
    if (params)
        if (const json::Value* meta = json::find(*params, "_meta"))
            meta_version = std::string(json::get_string(*meta, kMetaVersion.data()));
    if (!header_version.empty() && !meta_version.empty() && header_version != meta_version)
        return error_response(400, id, kHeaderMismatch,
                              "MCP-Protocol-Version header does not match _meta");
    const std::string declared = !header_version.empty() ? header_version : meta_version;
    if (!declared.empty() && !supported(declared)) {
        json::Document data(rapidjson::kObjectType);
        json::Value sup = json::array();
        for (std::string_view v : kSupportedVersions)
            sup.PushBack(json::str(v, data.GetAllocator()), data.GetAllocator());
        json::set(data, "supported", std::move(sup), data.GetAllocator());
        return error_response(400, id, kUnsupportedVersion,
                              "Unsupported protocol version " + declared, &data);
    }
    const bool modern = !declared.empty() && declared >= kModernSince;

    // Mirrored headers are validated when present; modern clients always send them.
    if (const std::string hm = http.header("Mcp-Method"); !hm.empty() && hm != method)
        return error_response(400, id, kHeaderMismatch,
                              "Mcp-Method header '" + hm + "' does not match body '" + method + "'");
    if (const std::string hn = http.header("Mcp-Name"); !hn.empty() && params) {
        const std::string decoded = base64::decode_header(hn);
        const std::string body_name(json::get_string(*params, "name"));
        if (decoded != body_name)
            return error_response(400, id, kHeaderMismatch,
                                  "Mcp-Name header does not match params.name");
    }

    if (!id) {
        HttpResponse r;  // notification: accepted, nothing to say
        r.status = 202;
        return r;
    }

    if (method == "tools/call") return call_tool(req, modern);
    return dispatch(*req, http, declared, modern);
}

HttpResponse Protocol::dispatch(const json::Document& req, const HttpRequest& /*http*/,
                                std::string_view declared, bool modern) const {
    const std::string method(json::get_string(req, "method"));
    const json::Value* id     = json::find(req, "id");
    const json::Value* params = json::find(req, "params");
    json::Document doc;
    json::Alloc& a = doc.GetAllocator();

    auto server_info = [&] {
        json::Value si = json::object();
        json::set(si, "name", info_.name, a);
        json::set(si, "title", info_.title, a);
        json::set(si, "version", info_.version, a);
        return si;
    };

    HttpResponse r;
    if (method == "server/discover") {
        json::Value res = json::object();
        json::set(res, "resultType", "complete", a);
        json::Value sup = json::array();
        for (std::string_view v : kSupportedVersions) sup.PushBack(json::str(v, a), a);
        json::set(res, "supportedVersions", std::move(sup), a);
        json::Value caps = json::object();
        json::set(caps, "tools", json::object(), a);
        json::set(res, "capabilities", std::move(caps), a);
        json::Value meta = json::object();
        json::set(meta, "io.modelcontextprotocol/serverInfo", server_info(), a);
        json::set(res, "_meta", std::move(meta), a);
        if (!info_.instructions.empty()) json::set(res, "instructions", info_.instructions, a);
        r.body = response_body(id, std::move(res), doc);
        return r;
    }
    if (method == "initialize") {
        std::string requested;
        if (params) requested = std::string(json::get_string(*params, "protocolVersion"));
        const std::string_view chosen = supported(requested) ? std::string_view(requested)
                                                             : kLegacyLatest;
        json::Value res = json::object();
        json::set(res, "protocolVersion", chosen, a);
        json::Value caps = json::object();
        json::Value tools = json::object();
        json::set(tools, "listChanged", false, a);
        json::set(caps, "tools", std::move(tools), a);
        json::set(res, "capabilities", std::move(caps), a);
        json::set(res, "serverInfo", server_info(), a);
        if (!info_.instructions.empty()) json::set(res, "instructions", info_.instructions, a);
        if (on_log) on_log("client initialised (protocol " + std::string(chosen) + ")");
        r.body = response_body(id, std::move(res), doc);
        return r;
    }
    if (method == "ping") {
        r.body = response_body(id, json::object(), doc);
        return r;
    }
    if (method == "tools/list") {
        json::Document list;
        json::parse(tools_.list_json(), list);
        json::Value res;
        res.CopyFrom(list, a);
        r.body = response_body(id, std::move(res), doc);
        return r;
    }
    (void)declared;
    return error_response(modern ? 404 : 200, id, kMethodNotFound,
                          "Method not found: " + method);
}

HttpResponse Protocol::call_tool(std::shared_ptr<json::Document> req, bool /*modern*/) const {
    const json::Value* id     = json::find(*req, "id");
    const json::Value* params = json::find(*req, "params");
    if (!params || !params->IsObject())
        return error_response(200, id, kInvalidParams, "tools/call needs params");
    const std::string name(json::get_string(*params, "name"));
    const ToolSpec* spec = tools_.find(name);
    if (!spec) return error_response(200, id, kInvalidParams, "Unknown tool: " + name);

    static const json::Value kEmptyArgs(rapidjson::kObjectType);
    const json::Value* args = json::find(*params, "arguments");
    if (!args || args->IsNull()) args = &kEmptyArgs;
    if (const std::string verr = ToolRegistry::validate_args(*spec, *args); !verr.empty())
        return error_response(200, id, kInvalidParams, verr);

    if (on_log) on_log("tools/call " + name);

    // Everything below runs on the main thread, once per frame until it yields a result.
    auto step = [req, spec, args]() -> std::optional<std::string> {
        std::optional<ToolResult> out;
        try {
            out = spec->handler(*args);
        } catch (const std::exception& e) {
            out = ToolResult::error(std::string("Tool failed: ") + e.what());
        } catch (...) {
            out = ToolResult::error("Tool failed with an unknown exception");
        }
        if (!out) return std::nullopt;

        json::Document doc;
        json::Alloc& a = doc.GetAllocator();
        json::Value res = json::object();
        json::Value content = json::array();
        for (const ToolContent& c : out->content) {
            json::Value o = json::object();
            if (c.kind == ToolContent::Kind::Text) {
                json::set(o, "type", "text", a);
                json::set(o, "text", c.text, a);
            } else {
                json::set(o, "type", "image", a);
                json::set(o, "data", c.data, a);
                json::set(o, "mimeType", c.mime, a);
            }
            content.PushBack(o, a);
        }
        json::set(res, "content", std::move(content), a);
        if (out->is_error) json::set(res, "isError", true, a);
        if (!out->structured.IsNull()) {
            json::Value sc;
            sc.CopyFrom(out->structured, a);
            json::set(res, "structuredContent", std::move(sc), a);
        }
        return response_body(json::find(*req, "id"), std::move(res), doc);
    };

    std::optional<std::string> body = dispatcher_.run(std::move(step), tool_timeout_);
    if (!body)
        return error_response(200, id, kServerBusy,
                              "Host did not answer within " +
                                  std::to_string(tool_timeout_.count()) + " ms");
    HttpResponse r;
    r.body = std::move(*body);
    return r;
}

}  // namespace mcp
