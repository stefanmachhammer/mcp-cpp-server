#include "mcp/protocol.hpp"

#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <random>
#include <utility>

#include "mcp/base64.hpp"

namespace mcp {

namespace {

constexpr std::array<std::string_view, 5> kSupportedVersions = {
    "2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
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

std::string new_session_id() {
    thread_local std::mt19937_64 rng{std::random_device{}() ^
                                     (std::uint64_t(std::random_device{}()) << 32)};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id(32, '0');
    for (int i = 0; i < 2; ++i) {
        std::uint64_t v = rng();
        for (int j = 0; j < 16; ++j, v >>= 4) id[i * 16 + j] = kHex[v & 15];
    }
    return id;
}

HttpResponse accepted() {
    HttpResponse r;
    r.status = 202;
    return r;
}

}  // namespace

struct Protocol::SseSession {
    std::mutex              mutex;
    std::condition_variable cv;
    std::deque<std::string> events;
    bool                    closed = false;

    void push(std::string event) {
        {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
        }
        cv.notify_one();
    }
    void close() {
        {
            std::lock_guard lock(mutex);
            closed = true;
        }
        cv.notify_all();
    }
};

Protocol::Protocol(const ToolRegistry& tools, Dispatcher& dispatcher, ServerInfo info,
                   ServerConfig config)
    : tools_(tools), dispatcher_(dispatcher), info_(std::move(info)),
      config_(std::move(config)) {}

std::vector<std::string> Protocol::paths() const {
    std::vector<std::string> p{config_.endpoint};
    if (config_.legacy_sse) {
        p.push_back(config_.sse_endpoint);
        p.push_back(config_.messages_endpoint);
    }
    return p;
}

void Protocol::shutdown() {
    closing_ = true;
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, s] : sessions_) s->close();
}

void Protocol::close_session(const std::string& id) {
    std::shared_ptr<SseSession> s;
    {
        std::lock_guard lock(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return;
        s = std::move(it->second);
        sessions_.erase(it);
    }
    s->close();
    if (on_log) on_log("SSE session closed");
}

bool Protocol::authorized(const HttpRequest& http) const {
    return config_.token.empty() || http.header("Authorization") == "Bearer " + config_.token;
}

HttpResponse Protocol::handle(const HttpRequest& http) {
    if (!origin_allowed(http.header("Origin")))
        return error_response(403, nullptr, kInvalidRequest, "Origin not allowed");
    if (http.path == config_.endpoint) return handle_streamable(http);
    if (config_.legacy_sse && http.path == config_.sse_endpoint) return open_sse(http);
    if (config_.legacy_sse && http.path == config_.messages_endpoint) return post_legacy(http);
    return error_response(404, nullptr, kInvalidRequest, "Unknown path");
}

HttpResponse Protocol::handle_streamable(const HttpRequest& http) {
    if (http.method != "POST") {
        HttpResponse r;
        r.status = 405;
        r.allow  = "POST";
        r.body   = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Use POST"}})";
        return r;
    }
    if (!authorized(http))
        return error_response(401, nullptr, kInvalidRequest, "Missing or invalid bearer token");
    return run_body(http, true);
}

HttpResponse Protocol::open_sse(const HttpRequest& http) {
    if (http.method != "GET") {
        HttpResponse r;
        r.status = 405;
        r.allow  = "GET";
        return r;
    }
    if (!authorized(http))
        return error_response(401, nullptr, kInvalidRequest, "Missing or invalid bearer token");
    if (closing_) return error_response(503, nullptr, kServerBusy, "Server is stopping");

    auto session = std::make_shared<SseSession>();
    const std::string id = new_session_id();
    {
        std::lock_guard lock(sessions_mutex_);
        if (static_cast<int>(sessions_.size()) >= config_.max_sse_sessions)
            return error_response(503, nullptr, kServerBusy, "Too many SSE sessions");
        sessions_.emplace(id, session);
    }
    session->push("event: endpoint\ndata: " + config_.messages_endpoint + "?sessionId=" + id +
                  "\n\n");
    if (on_log) on_log("SSE session opened (legacy transport)");

    HttpResponse r;
    r.content_type = "text/event-stream";
    r.stream = [session, last = std::chrono::steady_clock::now()](
                   const HttpResponse::Writer& write) mutable -> bool {
        std::deque<std::string> events;
        {
            std::unique_lock lock(session->mutex);
            session->cv.wait_for(lock, std::chrono::seconds(1),
                                 [&] { return session->closed || !session->events.empty(); });
            if (session->closed) return false;
            events.swap(session->events);
        }
        const auto now = std::chrono::steady_clock::now();
        if (events.empty() && now - last >= std::chrono::seconds(15))
            events.emplace_back(": keepalive\n\n");
        for (const std::string& e : events) {
            if (!write(e)) return false;
            last = now;
        }
        return true;
    };
    r.on_close = [this, id] { close_session(id); };
    return r;
}

HttpResponse Protocol::post_legacy(const HttpRequest& http) {
    if (http.method != "POST") {
        HttpResponse r;
        r.status = 405;
        r.allow  = "POST";
        return r;
    }
    if (!authorized(http))
        return error_response(401, nullptr, kInvalidRequest, "Missing or invalid bearer token");
    std::shared_ptr<SseSession> session;
    {
        std::lock_guard lock(sessions_mutex_);
        auto it = sessions_.find(http.query("sessionId"));
        if (it != sessions_.end()) session = it->second;
    }
    if (!session) return error_response(404, nullptr, kInvalidRequest, "Unknown or expired session");

    HttpResponse r = run_body(http, false);
    if (!r.body.empty()) session->push("event: message\ndata: " + r.body + "\n\n");
    return accepted();
}

HttpResponse Protocol::run_body(const HttpRequest& http, bool single_headers) const {
    auto doc = std::make_shared<json::Document>();
    std::string perr;
    if (!json::parse(http.body, *doc, &perr))
        return error_response(400, nullptr, kParseError, "Parse error: " + perr);
    if (!doc->IsArray()) return run_message(doc, *doc, http, single_headers);

    // Batch (protocol 2025-03-26): one array of responses, notifications omitted.
    if (doc->Empty()) return error_response(400, nullptr, kInvalidRequest, "Empty batch");
    std::string body = "[";
    bool any = false;
    for (const json::Value& msg : doc->GetArray()) {
        HttpResponse r = run_message(doc, msg, http, false);
        if (r.body.empty()) continue;
        if (any) body += ',';
        body += r.body;
        any = true;
    }
    if (!any) return accepted();
    HttpResponse r;
    r.body = body + "]";
    return r;
}

HttpResponse Protocol::run_message(const std::shared_ptr<json::Document>& owner,
                                   const json::Value& msg, const HttpRequest& http,
                                   bool single) const {
    if (!msg.IsObject() || json::get_string(msg, "jsonrpc") != "2.0" ||
        !json::has(msg, "method") || !json::find(msg, "method")->IsString()) {
        // A response or an error object from the client (e.g. to a ping) needs no answer.
        if (msg.IsObject() && (json::has(msg, "result") || json::has(msg, "error")))
            return accepted();
        return error_response(400, nullptr, kInvalidRequest,
                              "Body must be a JSON-RPC 2.0 request, notification or batch");
    }

    const std::string method(json::get_string(msg, "method"));
    const json::Value* id     = json::find(msg, "id");
    const json::Value* params = json::find(msg, "params");

    // Version: header and _meta must agree; either alone is accepted.
    const std::string header_version = http.header("MCP-Protocol-Version");
    std::string meta_version;
    if (params && params->IsObject())
        if (const json::Value* meta = json::find(*params, "_meta"); meta && meta->IsObject())
            meta_version = std::string(json::get_string(*meta, kMetaVersion.data()));
    if (!header_version.empty() && !meta_version.empty() && header_version != meta_version)
        return error_response(400, id, kHeaderMismatch,
                              "MCP-Protocol-Version header does not match _meta");
    const std::string declared = !header_version.empty() ? header_version : meta_version;
    if (!declared.empty() && !supported(declared) && method != "initialize") {
        json::Document data(rapidjson::kObjectType);
        json::Value sup = json::array();
        for (std::string_view v : kSupportedVersions)
            sup.PushBack(json::str(v, data.GetAllocator()), data.GetAllocator());
        json::set(data, "supported", std::move(sup), data.GetAllocator());
        return error_response(400, id, kUnsupportedVersion,
                              "Unsupported protocol version " + declared, &data);
    }
    const bool modern = !declared.empty() && declared >= kModernSince;

    // Mirrored headers describe a single message; they are validated when present.
    if (single) {
        if (const std::string hm = http.header("Mcp-Method"); !hm.empty() && hm != method)
            return error_response(400, id, kHeaderMismatch,
                                  "Mcp-Method header '" + hm + "' does not match body '" + method + "'");
        if (const std::string hn = http.header("Mcp-Name"); !hn.empty() && params &&
                                                             params->IsObject()) {
            const std::string decoded = base64::decode_header(hn);
            const std::string body_name(json::get_string(*params, "name"));
            if (decoded != body_name)
                return error_response(400, id, kHeaderMismatch,
                                      "Mcp-Name header does not match params.name");
        }
    }

    if (!id) return accepted();  // notification
    if (method == "tools/call") return call_tool(owner, msg);
    return dispatch(msg, modern);
}

HttpResponse Protocol::dispatch(const json::Value& msg, bool modern) const {
    const std::string method(json::get_string(msg, "method"));
    const json::Value* id     = json::find(msg, "id");
    const json::Value* params = json::find(msg, "params");
    json::Document doc;
    json::Alloc& a = doc.GetAllocator();

    auto server_info = [&] {
        json::Value si = json::object();
        json::set(si, "name", info_.name, a);
        json::set(si, "title", info_.title, a);
        json::set(si, "version", info_.version, a);
        return si;
    };
    auto empty_list = [&](const char* key) {
        json::Value res = json::object();
        json::set(res, key, json::array(), a);
        return res;
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
        if (params && params->IsObject())
            requested = std::string(json::get_string(*params, "protocolVersion"));
        const std::string_view chosen = supported(requested) ? std::string_view(requested)
                                                             : kLegacyLatest;
        std::string client;
        if (params && params->IsObject())
            if (const json::Value* ci = json::find(*params, "clientInfo"); ci && ci->IsObject())
                client = std::string(json::get_string(*ci, "name"));
        json::Value res = json::object();
        json::set(res, "protocolVersion", chosen, a);
        json::Value caps = json::object();
        json::Value tools = json::object();
        json::set(tools, "listChanged", false, a);
        json::set(caps, "tools", std::move(tools), a);
        json::set(res, "capabilities", std::move(caps), a);
        json::set(res, "serverInfo", server_info(), a);
        if (!info_.instructions.empty()) json::set(res, "instructions", info_.instructions, a);
        if (on_log)
            on_log("client initialised: " + (client.empty() ? std::string("unknown") : client) +
                   " (protocol " + std::string(chosen) + ")");
        r.body = response_body(id, std::move(res), doc);
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
    // Several clients probe these whatever the capabilities say; empty answers
    // keep them from logging the server as broken.
    if (method == "ping" || method == "logging/setLevel") {
        r.body = response_body(id, json::object(), doc);
        return r;
    }
    if (method == "resources/list") {
        r.body = response_body(id, empty_list("resources"), doc);
        return r;
    }
    if (method == "resources/templates/list") {
        r.body = response_body(id, empty_list("resourceTemplates"), doc);
        return r;
    }
    if (method == "prompts/list") {
        r.body = response_body(id, empty_list("prompts"), doc);
        return r;
    }
    return error_response(modern ? 404 : 200, id, kMethodNotFound,
                          "Method not found: " + method);
}

HttpResponse Protocol::call_tool(const std::shared_ptr<json::Document>& owner,
                                 const json::Value& msg) const {
    const json::Value* id     = json::find(msg, "id");
    const json::Value* params = json::find(msg, "params");
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
    // owner keeps msg, id and args alive for as long as the step is queued.
    auto step = [owner, id, spec, args]() -> std::optional<std::string> {
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
        return response_body(id, std::move(res), doc);
    };

    std::optional<std::string> body = dispatcher_.run(std::move(step), config_.tool_timeout);
    if (!body)
        return error_response(200, id, kServerBusy,
                              "Host did not answer within " +
                                  std::to_string(config_.tool_timeout.count()) + " ms");
    HttpResponse r;
    r.body = std::move(*body);
    return r;
}

}  // namespace mcp
