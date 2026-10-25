#include "Auth.hpp"

#include <algorithm>
#include <cctype>
#include "std.hpp"

static std::optional<std::string> base64_decode(std::string_view s) {
    auto value = [](char c) -> i32 {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    while (s.ends_with('='))
        s.remove_suffix(1);
    std::string out;
    out.reserve(s.size() * 3 / 4);
    u32 bits = 0;
    i32 bit_count = 0;
    for (char c : s) {
        i32 v = value(c);
        if (v < 0)
            return std::nullopt;
        bits = (bits << 6) | static_cast<u32>(v);
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            out += static_cast<char>((bits >> bit_count) & 0xFF);
        }
    }
    return out;
}

std::optional<std::string> basic_auth_password(std::string_view authorization) {
    constexpr std::string_view scheme = "basic ";
    if (authorization.size() < scheme.size())
        return std::nullopt;
    // the scheme name is case-insensitive
    bool is_basic = std::ranges::equal(authorization.substr(0, scheme.size()), scheme,
        [](char a, char b) { return std::tolower(static_cast<u8>(a)) == b; });
    if (!is_basic)
        return std::nullopt;
    std::optional<std::string> decoded = base64_decode(authorization.substr(scheme.size()));
    if (!decoded)
        return std::nullopt;
    size_t colon = decoded->find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    return decoded->substr(colon + 1);
}

/// time doesn't depend on where the first different character is, so it doesn't help guessing
static bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    u8 diff = 0;
    for (size_t i = 0; i < a.size(); i++)
        diff |= static_cast<u8>(a[i] ^ b[i]);
    return diff == 0;
}

static bool matches(const std::optional<std::string>& expected, const std::optional<std::string>& password) {
    return expected && password && constant_time_equal(*expected, *password);
}

bool is_authorized(const Share& share, Access access, const std::optional<std::string>& password) {
    if (access == Access::read)
        return !share.read_password || matches(share.read_password, password) || matches(share.write_password, password);
    return matches(share.write_password, password);
}
