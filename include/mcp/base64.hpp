#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcp::base64 {

inline std::string encode(const std::uint8_t* data, std::size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
    }
    if (i < len) {
        std::uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kTable[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

inline std::string encode(std::string_view s) {
    return encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

inline std::string decode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// Header values may be wrapped as =?base64?...?= when not header-safe.
inline std::string decode_header(std::string_view v) {
    constexpr std::string_view kPrefix = "=?base64?";
    constexpr std::string_view kSuffix = "?=";
    if (v.size() >= kPrefix.size() + kSuffix.size() &&
        v.substr(0, kPrefix.size()) == kPrefix &&
        v.substr(v.size() - kSuffix.size()) == kSuffix) {
        return decode(v.substr(kPrefix.size(), v.size() - kPrefix.size() - kSuffix.size()));
    }
    return std::string(v);
}

}  // namespace mcp::base64
