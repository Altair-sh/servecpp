#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include "Path.hpp"

/// Content-Type by file extension, application/octet-stream if unknown.
/// html, svg, xml and js are deliberately missing: uploaded files must not run scripts on this site.
inline std::string_view mime_type(const std::filesystem::path& p) {
    static constexpr std::pair<std::string_view, std::string_view> types[] = {
        { ".txt",  "text/plain; charset=utf-8" },
        { ".md",   "text/plain; charset=utf-8" },
        { ".log",  "text/plain; charset=utf-8" },
        { ".csv",  "text/csv; charset=utf-8" },
        { ".json", "application/json" },
        { ".pdf",  "application/pdf" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".webp", "image/webp" },
        { ".avif", "image/avif" },
        { ".bmp",  "image/bmp" },
        { ".ico",  "image/x-icon" },
        { ".mp4",  "video/mp4" },
        { ".m4v",  "video/mp4" },
        { ".webm", "video/webm" },
        { ".mkv",  "video/x-matroska" },
        { ".mov",  "video/quicktime" },
        { ".mp3",  "audio/mpeg" },
        { ".m4a",  "audio/mp4" },
        { ".ogg",  "audio/ogg" },
        { ".opus", "audio/opus" },
        { ".wav",  "audio/wav" },
        { ".flac", "audio/flac" },
        { ".zip",  "application/zip" },
        { ".gz",   "application/gzip" },
        { ".7z",   "application/x-7z-compressed" },
        { ".tar",  "application/x-tar" },
    };
    std::string ext = path_utf8(p.extension());
    std::ranges::transform(ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    for (auto& [e, type] : types)
        if (e == ext)
            return type;
    return "application/octet-stream";
}
