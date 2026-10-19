#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// Thin conveniences over RapidJSON so tool code stays readable: typed getters
// that tolerate a missing or mistyped member, and setters that copy strings.
namespace mcp::json {

using rapidjson::Document;
using rapidjson::Value;
using Alloc = Document::AllocatorType;

inline std::string dump(const Value& v) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    v.Accept(w);
    return std::string(sb.GetString(), sb.GetSize());
}

inline bool parse(std::string_view text, Document& out, std::string* err = nullptr) {
    out.Parse(text.data(), text.size());
    if (!out.HasParseError()) return true;
    if (err) {
        *err = std::string(rapidjson::GetParseError_En(out.GetParseError())) +
               " at offset " + std::to_string(out.GetErrorOffset());
    }
    return false;
}

inline Value str(std::string_view s, Alloc& a) {
    return Value(s.data(), static_cast<rapidjson::SizeType>(s.size()), a);
}

inline const Value* find(const Value& obj, const char* key) {
    if (!obj.IsObject()) return nullptr;
    const auto it = obj.FindMember(key);
    return it == obj.MemberEnd() ? nullptr : &it->value;
}

inline bool has(const Value& obj, const char* key) { return find(obj, key) != nullptr; }

inline std::string_view get_string(const Value& obj, const char* key,
                                   std::string_view def = {}) {
    const Value* v = find(obj, key);
    return v && v->IsString() ? std::string_view(v->GetString(), v->GetStringLength()) : def;
}

inline double get_number(const Value& obj, const char* key, double def = 0.0) {
    const Value* v = find(obj, key);
    return v && v->IsNumber() ? v->GetDouble() : def;
}

inline int get_int(const Value& obj, const char* key, int def = 0) {
    const Value* v = find(obj, key);
    if (!v) return def;
    if (v->IsInt()) return v->GetInt();
    if (v->IsNumber()) return static_cast<int>(v->GetDouble());
    return def;
}

inline bool get_bool(const Value& obj, const char* key, bool def = false) {
    const Value* v = find(obj, key);
    return v && v->IsBool() ? v->GetBool() : def;
}

// Overloads so call sites read set(o, "k", value) regardless of the value type.
inline void set(Value& obj, const char* key, Value v, Alloc& a) {
    obj.AddMember(Value(key, a), v, a);
}
inline void set(Value& obj, const char* key, std::string_view s, Alloc& a) {
    obj.AddMember(Value(key, a), str(s, a), a);
}
inline void set(Value& obj, const char* key, const char* s, Alloc& a) {
    set(obj, key, std::string_view(s), a);
}
inline void set(Value& obj, const char* key, const std::string& s, Alloc& a) {
    set(obj, key, std::string_view(s), a);
}
inline void set(Value& obj, const char* key, bool b, Alloc& a) {
    obj.AddMember(Value(key, a), Value(b), a);
}
inline void set(Value& obj, const char* key, int i, Alloc& a) {
    obj.AddMember(Value(key, a), Value(i), a);
}
inline void set(Value& obj, const char* key, std::int64_t i, Alloc& a) {
    obj.AddMember(Value(key, a), Value(i), a);
}
inline void set(Value& obj, const char* key, std::uint64_t i, Alloc& a) {
    obj.AddMember(Value(key, a), Value(i), a);
}
inline void set(Value& obj, const char* key, double d, Alloc& a) {
    obj.AddMember(Value(key, a), Value(d), a);
}
inline void set(Value& obj, const char* key, float f, Alloc& a) {
    obj.AddMember(Value(key, a), Value(static_cast<double>(f)), a);
}

inline Value object() { return Value(rapidjson::kObjectType); }
inline Value array()  { return Value(rapidjson::kArrayType); }

inline Value vec3(float x, float y, float z, Alloc& a) {
    Value v(rapidjson::kArrayType);
    v.PushBack(static_cast<double>(x), a).PushBack(static_cast<double>(y), a)
     .PushBack(static_cast<double>(z), a);
    return v;
}

// Reads a 3-element numeric array; returns false and leaves out untouched otherwise.
inline bool read_vec3(const Value* v, float out[3]) {
    if (!v || !v->IsArray() || v->Size() != 3) return false;
    for (rapidjson::SizeType i = 0; i < 3; ++i)
        if (!(*v)[i].IsNumber()) return false;
    for (rapidjson::SizeType i = 0; i < 3; ++i)
        out[i] = static_cast<float>((*v)[i].GetDouble());
    return true;
}

}  // namespace mcp::json
