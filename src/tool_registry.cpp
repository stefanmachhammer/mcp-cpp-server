#include "mcp/tool_registry.hpp"

#include <stdexcept>
#include <utility>

#include "mcp/base64.hpp"

namespace mcp {

ToolResult ToolResult::text(std::string s) {
    ToolResult r;
    r.content.push_back({ToolContent::Kind::Text, std::move(s), {}, {}});
    return r;
}

ToolResult ToolResult::error(std::string message) {
    ToolResult r = text(std::move(message));
    r.is_error = true;
    return r;
}

ToolResult ToolResult::json(json::Document doc) {
    ToolResult r = text(json::dump(doc));
    r.structured = std::move(doc);
    return r;
}

ToolResult ToolResult::image_png(const std::uint8_t* bytes, std::size_t len,
                                 std::string caption) {
    ToolResult r;
    if (!caption.empty())
        r.content.push_back({ToolContent::Kind::Text, std::move(caption), {}, {}});
    r.content.push_back({ToolContent::Kind::Image, {}, base64::encode(bytes, len), "image/png"});
    return r;
}

bool ToolRegistry::portable_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

void ToolRegistry::add(ToolSpec spec) {
    if (!portable_name(spec.name))
        throw std::invalid_argument("tool name must match [A-Za-z0-9_-]{1,64}: '" + spec.name + "'");
    if (find(spec.name)) throw std::invalid_argument("duplicate tool: " + spec.name);
    json::Document schema;
    std::string err;
    if (!json::parse(spec.input_schema, schema, &err) || !schema.IsObject())
        throw std::invalid_argument("bad input schema for " + spec.name + ": " + err);
    if (json::get_string(schema, "type") != "object")
        throw std::invalid_argument("input schema of " + spec.name + " must have \"type\":\"object\"");
    if (!spec.handler) throw std::invalid_argument("tool without handler: " + spec.name);
    tools_.push_back(std::move(spec));
    list_cache_.clear();
}

const ToolSpec* ToolRegistry::find(std::string_view name) const noexcept {
    for (const ToolSpec& t : tools_)
        if (t.name == name) return &t;
    return nullptr;
}

const std::string& ToolRegistry::list_json() const {
    if (!list_cache_.empty()) return list_cache_;
    json::Document doc(rapidjson::kObjectType);
    json::Alloc& a = doc.GetAllocator();
    json::Value arr = json::array();
    for (const ToolSpec& t : tools_) {
        json::Value o = json::object();
        json::set(o, "name", t.name, a);
        if (!t.title.empty()) json::set(o, "title", t.title, a);
        json::set(o, "description", t.description, a);
        json::Document schema;
        json::parse(t.input_schema, schema);  // validated in add()
        json::Value sv;
        sv.CopyFrom(schema, a);
        json::set(o, "inputSchema", std::move(sv), a);
        json::Value ann = json::object();
        json::set(ann, "readOnlyHint", t.read_only, a);
        json::set(ann, "destructiveHint", t.destructive, a);
        json::set(ann, "idempotentHint", t.idempotent, a);
        json::set(ann, "openWorldHint", false, a);
        json::set(o, "annotations", std::move(ann), a);
        arr.PushBack(o, a);
    }
    json::set(doc, "tools", std::move(arr), a);
    list_cache_ = json::dump(doc);
    return list_cache_;
}

std::string ToolRegistry::validate_args(const ToolSpec& spec, const json::Value& args) {
    json::Document schema;
    json::parse(spec.input_schema, schema);
    if (!args.IsObject()) return "arguments must be an object";

    if (const json::Value* req = json::find(schema, "required"); req && req->IsArray()) {
        for (const json::Value& r : req->GetArray()) {
            if (!r.IsString()) continue;
            if (!json::has(args, r.GetString()))
                return std::string("missing required argument: ") + r.GetString();
        }
    }
    const json::Value* props = json::find(schema, "properties");
    if (!props || !props->IsObject()) return {};
    for (auto it = args.MemberBegin(); it != args.MemberEnd(); ++it) {
        const json::Value* p = json::find(*props, it->name.GetString());
        if (!p) continue;  // unknown keys are tolerated
        const std::string_view type = json::get_string(*p, "type");
        const json::Value& v = it->value;
        bool ok = true;
        if (type == "string")       ok = v.IsString();
        else if (type == "integer") ok = v.IsInt() || v.IsInt64();
        else if (type == "number")  ok = v.IsNumber();
        else if (type == "boolean") ok = v.IsBool();
        else if (type == "array")   ok = v.IsArray();
        else if (type == "object")  ok = v.IsObject();
        if (!ok)
            return std::string("argument '") + it->name.GetString() + "' must be a " +
                   std::string(type);
    }
    return {};
}

}  // namespace mcp
