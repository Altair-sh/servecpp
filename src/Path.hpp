#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/// UTF-8 string -> path. On Windows a path from std::string is interpreted in the ANSI code page, not UTF-8.
std::filesystem::path utf8_path(std::string_view s);
/// path -> UTF-8 string
std::string path_utf8(const std::filesystem::path& p);

/// decodes %xx, nullopt on invalid %xx, %00 or invalid UTF-8 in the result
std::optional<std::string> percent_decode(std::string_view s);
/// encodes everything except unreserved characters and '/'
std::string percent_encode_path(std::string_view s);
std::string html_escape(std::string_view s);
bool is_valid_utf8(std::string_view s);

/// Maps a decoded path inside a share ("/dir/file.txt") to a file under root.
/// nullopt if any segment is "..", contains '\', ':' or control characters, is a Windows reserved name,
/// or the result points outside root (through a symlink).
/// root must be canonical.
std::optional<std::filesystem::path> resolve(const std::filesystem::path& root, std::string_view rel);
