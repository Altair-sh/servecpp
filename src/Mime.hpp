#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include "Path.hpp"

/// Content-Type by file extension, application/octet-stream if unknown.
/// html, svg, xml and js are deliberately missing: uploaded files must not run scripts on this site.
inline std::string_view mime_type(const std::filesystem::path& p) {
    struct MimeType {
        std::string_view extension;
        std::string_view type;
    };
    // sorted by extension for binary search
    static constexpr MimeType types[] = {
        { ".7z",   "application/x-7z-compressed" },
        { ".avif", "image/avif" },
        { ".bmp",  "image/bmp" },
        { ".csv",  "text/csv; charset=utf-8" },
        { ".flac", "audio/flac" },
        { ".gif",  "image/gif" },
        { ".gz",   "application/gzip" },
        { ".ico",  "image/x-icon" },
        { ".jpeg", "image/jpeg" },
        { ".jpg",  "image/jpeg" },
        { ".json", "application/json" },
        { ".log",  "text/plain; charset=utf-8" },
        { ".m4a",  "audio/mp4" },
        { ".m4v",  "video/mp4" },
        { ".md",   "text/plain; charset=utf-8" },
        { ".mkv",  "video/x-matroska" },
        { ".mov",  "video/quicktime" },
        { ".mp3",  "audio/mpeg" },
        { ".mp4",  "video/mp4" },
        { ".ogg",  "audio/ogg" },
        { ".opus", "audio/opus" },
        { ".pdf",  "application/pdf" },
        { ".png",  "image/png" },
        { ".tar",  "application/x-tar" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".wav",  "audio/wav" },
        { ".webm", "video/webm" },
        { ".webp", "image/webp" },
        { ".zip",  "application/zip" },
    };
    static_assert(std::ranges::is_sorted(types, {}, &MimeType::extension), "keep types sorted by extension");

    std::string ext = path_utf8(p.extension());
    std::ranges::transform(ext, ext.begin(), [](char c) { 
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); 
    });
    // binary search
    auto it = std::ranges::lower_bound(
        types, std::string_view(ext), {}, &MimeType::extension);
    if (it != std::ranges::end(types) && it->extension == ext)
        return it->type;
    return "application/octet-stream";
}
