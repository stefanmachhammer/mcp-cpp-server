#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mcp/json.hpp"

namespace mcp {

struct ToolContent {
    enum class Kind { Text, Image };
    Kind        kind = Kind::Text;
    std::string text;  // Text
    std::string data;  // Image: base64 payload
    std::string mime;  // Image
};

// What a tool hands back. structured is sent as structuredContent when it is
// not null; json() fills both it and a text rendering so every client sees it.
struct ToolResult {
    std::vector<ToolContent> content;
    bool                     is_error = false;
    json::Document           structured;

    ToolResult() = default;
    ToolResult(ToolResult&&) noexcept            = default;
    ToolResult& operator=(ToolResult&&) noexcept = default;

    static ToolResult text(std::string s);
    static ToolResult error(std::string message);
    static ToolResult json(json::Document doc);
    static ToolResult image_png(const std::uint8_t* bytes, std::size_t len,
                                std::string caption = {});
};

// The handler runs on the host's main thread. Returning nullopt asks to be
// called again next frame with the same arguments (multi-frame tools).
using ToolHandler = std::function<std::optional<ToolResult>(const json::Value& args)>;

struct ToolSpec {
    std::string name;
    std::string title;
    std::string description;
    std::string input_schema = R"({"type":"object","properties":{}})";
    bool        read_only   = false;
    bool        destructive = false;
    bool        idempotent  = false;
    ToolHandler handler;
};

class ToolRegistry {
public:
    // Throws std::invalid_argument on a duplicate name or an unparsable schema.
    void add(ToolSpec spec);

    const ToolSpec* find(std::string_view name) const noexcept;
    const std::vector<ToolSpec>& tools() const noexcept { return tools_; }

    // Serialised tools/list result; cached because the set is fixed after start.
    const std::string& list_json() const;

    // Top-level required/type check against the input schema; empty when fine.
    static std::string validate_args(const ToolSpec& spec, const json::Value& args);

private:
    std::vector<ToolSpec> tools_;
    mutable std::string   list_cache_;
};

}  // namespace mcp
