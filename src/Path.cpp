#include "Path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include "std.hpp"

namespace fs = std::filesystem;

fs::path utf8_path(std::string_view s) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string path_utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

static i32 hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::string> percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size())
            return std::nullopt;
        i32 hi = hex_value(s[i + 1]);
        i32 lo = hex_value(s[i + 2]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
            return std::nullopt;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    if (!is_valid_utf8(out))
        return std::nullopt;
    return out;
}

std::string percent_encode_path(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        u8 c = static_cast<u8>(ch);
        bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        if (unreserved) {
            out += ch;
        }
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        u8 c = static_cast<u8>(s[i]);
        size_t len;
        u32 cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > s.size())
            return false;
        for (size_t j = 1; j < len; j++) {
            u8 cc = static_cast<u8>(s[i + j]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // overlong encodings, surrogates, out of range
        static constexpr u32 min_cp[] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < min_cp[len] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            return false;
        i += len;
    }
    return true;
}

static bool is_reserved_windows_name(std::string_view seg) {
    // "CON", "con.txt", "COM1.tar.gz"... refer to devices on Windows
    std::string base(seg.substr(0, seg.find('.')));
    std::ranges::transform(base, base.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<u8>(c))); });
    static constexpr std::array<std::string_view, 4> names = { "CON", "PRN", "AUX", "NUL" };
    if (std::ranges::find(names, base) != names.end())
        return true;
    return base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT"))
        && base[3] >= '0' && base[3] <= '9';
}

static bool is_safe_segment(std::string_view seg) {
    if (seg == "..")
        return false;
    // Windows strips trailing dots and spaces, "a.txt." would open "a.txt"
    if (seg.back() == '.' || seg.back() == ' ')
        return false;
    for (char ch : seg) {
        u8 c = static_cast<u8>(ch);
        if (c < 0x20 || c == 0x7F)
            return false;
        // invalid in Windows file names, ':' also selects NTFS streams
        if (std::string_view("\\:*?\"<>|").find(ch) != std::string_view::npos)
            return false;
    }
    return !is_reserved_windows_name(seg);
}

std::optional<fs::path> resolve(const fs::path& root, std::string_view rel) {
    fs::path p = root;
    for (auto part : std::views::split(rel, '/')) {
        std::string_view seg(part.begin(), part.end());
        if (seg.empty() || seg == ".")
            continue;
        if (!is_safe_segment(seg))
            return std::nullopt;
        p /= utf8_path(seg);
    }
    // resolves symlinks of the existing part of the path, the file itself may not exist yet (upload)
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (ec)
        return std::nullopt;
    auto [root_it, canon_it] = std::mismatch(root.begin(), root.end(), canon.begin(), canon.end());
    if (root_it != root.end())
        return std::nullopt;
    return canon;
}
