#include "Download.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <memory>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "Listing.hpp"
#include "Logger.hpp"
#include "Mime.hpp"
#include "Path.hpp"
#include "TempFile.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace fs = std::filesystem;
using asio::awaitable;

namespace {

std::string http_date(fs::file_time_type t) {
    auto sys = std::chrono::floor<std::chrono::seconds>(std::chrono::file_clock::to_sys(t));
    return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", sys);
}

/// filename for "Save as": ASCII fallback + the real name in UTF-8 (RFC 6266)
std::string content_disposition(const fs::path& file) {
    std::string name = path_utf8(file.filename());
    std::string ascii;
    for (char ch : name) {
        u8 c = static_cast<u8>(ch);
        if (c >= 0x80 && c < 0xC0)
            continue;  // UTF-8 continuation byte, the character is already replaced by its first byte
        ascii += (c < 0x20 || c >= 0x7F || ch == '"' || ch == '\\') ? '_' : ch;
    }
    return std::format("inline; filename=\"{}\"; filename*=UTF-8''{}", ascii, percent_encode_path(name));
}

struct ByteRange {
    enum Kind { full, partial, unsatisfiable } kind;
    u64 first = 0;
    u64 length = 0;
};

/// One range: "bytes=a-b", "bytes=a-" or "bytes=-n".
/// Malformed and multi-range headers are ignored and the whole file is sent, RFC 9110 allows it.
ByteRange parse_range(std::string_view header, u64 size) {
    ByteRange full{ ByteRange::full, 0, size };
    constexpr std::string_view unit = "bytes=";
    if (!header.starts_with(unit) || header.find(',') != std::string_view::npos)
        return full;
    header.remove_prefix(unit.size());
    size_t dash = header.find('-');
    if (dash == std::string_view::npos)
        return full;
    auto parse = [](std::string_view s, u64& v) {
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        return !s.empty() && ec == std::errc() && end == s.data() + s.size();
    };
    std::string_view first_str = header.substr(0, dash);
    std::string_view last_str = header.substr(dash + 1);

    // suffix: the last n bytes
    if (first_str.empty()) {
        u64 n;
        if (!parse(last_str, n))
            return full;
        if (n == 0 || size == 0)
            return { ByteRange::unsatisfiable };
        n = std::min(n, size);
        return { ByteRange::partial, size - n, n };
    }

    u64 first, last = size - 1;
    if (!parse(first_str, first) || (!last_str.empty() && (!parse(last_str, last) || last < first)))
        return full;
    if (first >= size)
        return { ByteRange::unsatisfiable };
    last = std::min(last, size - 1);
    return { ByteRange::partial, first, last - first + 1 };
}

awaitable<void> send_file(Exchange& ex, const fs::path& file) {
    beast::error_code ec;
    beast::file f;
    f.open(path_utf8(file).c_str(), beast::file_mode::scan, ec);
    if (ec) {
        co_await send_message(ex, http::status::not_found, "file not found");
        co_return;
    }
    u64 size = f.size(ec);
    std::error_code fs_ec;
    fs::file_time_type mtime = fs::last_write_time(file, fs_ec);
    if (ec || fs_ec) {
        co_await send_message(ex, http::status::internal_server_error, "can't read the file");
        co_return;
    }
    std::string last_modified = http_date(mtime);
    const auto& req = ex.parser.get();

    // clients send back the Last-Modified value they received, an exact match is enough
    if (std::string_view(req[http::field::if_modified_since]) == last_modified) {
        Response res{ http::status::not_modified, ex.version };
        res.set(http::field::last_modified, last_modified);
        co_await send_header(ex, res);
        co_return;
    }

    ByteRange range = parse_range(req[http::field::range], size);
    if (range.kind == ByteRange::unsatisfiable) {
        auto res = make_response(ex, http::status::range_not_satisfiable, "range is outside the file\n", text_plain);
        res.set(http::field::content_range, std::format("bytes */{}", size));
        co_await send(ex, std::move(res));
        co_return;
    }

    Response res{
        range.kind == ByteRange::partial ? http::status::partial_content : http::status::ok, ex.version };
    res.set(http::field::content_type, mime_type(file));
    res.set(http::field::content_disposition, content_disposition(file));
    res.set(http::field::last_modified, last_modified);
    res.set(http::field::accept_ranges, "bytes");
    // the browser must not guess another type from the content (an uploaded file with HTML inside)
    res.set("X-Content-Type-Options", "nosniff");
    if (range.kind == ByteRange::partial)
        res.set(http::field::content_range, std::format("bytes {}-{}/{}", range.first, range.first + range.length - 1, size));
    res.content_length(range.length);
    co_await send_header(ex, res);
    if (ex.head || range.length == 0)
        co_return;

    f.seek(range.first, ec);
    auto chunk = std::make_unique_for_overwrite<char[]>(file_chunk_size);
    u64 remain = range.length;
    while (remain > 0) {
        size_t n = f.read(chunk.get(), std::min<u64>(remain, file_chunk_size), ec);
        if (ec || n == 0) {
            // Content-Length is already sent, the only way to report the error is to close the connection
            log_warning("can't read {}: {}", path_utf8(file), ec ? ec.message() : "file was truncated");
            ex.keep_alive = false;
            co_return;
        }
        co_await send_body(ex, std::string_view(chunk.get(), n));
        remain -= n;
    }
}

} // namespace

awaitable<void> handle_get(Exchange& ex, const Share& share, std::string_view url_path, const fs::path& file) {
    std::error_code ec;
    fs::file_status st = fs::status(file, ec);
    if (fs::is_directory(st))
        co_await send_listing(ex, share, url_path, file);
    else if (fs::is_regular_file(st) && !is_temp_file(path_utf8(file.filename())))
        co_await send_file(ex, file);
    else
        co_await send_message(ex, http::status::not_found, "file not found");
}
