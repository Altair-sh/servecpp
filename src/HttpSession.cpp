#include "HttpSession.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <memory>
#include <vector>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "Auth.hpp"
#include "Logger.hpp"
#include "Mime.hpp"
#include "Path.hpp"
#include "TempFile.hpp"
#include "std.hpp"
// generated from templates/*.htcpp
#include "ListingPage.h"
#include "SharesPage.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using asio::awaitable;
using asio::use_awaitable;
using asio::ip::tcp;

namespace {

constexpr auto io_timeout = 30s;        // idle keep-alive, request header, each body chunk
constexpr auto auth_fail_delay = 1s;    // slows down password guessing
constexpr size_t header_limit = 8 * 1024;
constexpr size_t file_chunk_size = 256 * 1024;
constexpr std::string_view text_plain = "text/plain; charset=utf-8";
constexpr std::string_view text_html = "text/html; charset=utf-8";

using RequestParser = http::request_parser<http::empty_body>;

/// One request and its response. Handlers fill status, bytes and keep_alive.
struct Exchange {
    beast::tcp_stream& stream;
    beast::flat_buffer& buffer;
    RequestParser& parser;          // moved into a file_body parser by PUT, use version and head instead
    const Config& config;
    unsigned version;
    bool head;
    bool body_pending;              // request body not read yet: the connection can't be reused
    http::status status = http::status::ok;
    u64 bytes = 0;                  // response body bytes sent
    bool keep_alive = true;
};

bool is_disconnect(const boost::system::error_code& ec) {
    return ec == http::error::end_of_stream || ec == http::error::partial_message
        || ec == beast::error::timeout || ec == asio::error::eof
        || ec == asio::error::connection_reset || ec == asio::error::connection_aborted
        || ec == asio::error::broken_pipe || ec == asio::error::operation_aborted;
}

template <class Body>
void set_common_headers(Exchange& ex, http::response<Body>& res) {
    res.set(http::field::server, "servecpp");
    if (ex.body_pending)
        ex.keep_alive = false;
    res.keep_alive(ex.keep_alive);
    ex.status = res.result();
}

http::response<http::string_body> make_response(const Exchange& ex, http::status status, std::string body, std::string_view type) {
    http::response<http::string_body> res{status, ex.version};
    res.set(http::field::content_type, type);
    res.body() = std::move(body);
    return res;
}

awaitable<void> send(Exchange& ex, http::response<http::string_body> res) {
    set_common_headers(ex, res);
    res.prepare_payload();
    // HEAD gets the same Content-Length, but no body
    if (ex.head)
        res.body().clear();
    ex.bytes = res.body().size();
    ex.stream.expires_after(io_timeout);
    co_await http::async_write(ex.stream, res, use_awaitable);
}

/// sends only the header, the body is written by the caller
awaitable<void> send_header(Exchange& ex, http::response<http::empty_body>& res) {
    set_common_headers(ex, res);
    ex.stream.expires_after(io_timeout);
    co_await http::async_write(ex.stream, res, use_awaitable);
}

/// The status says what happened, the body says why, readable by people (curl) and the upload form.
/// The reason phrase isn't used for that: HTTP/2 between the browser and the proxy doesn't have it.
awaitable<void> send_message(Exchange& ex, http::status status, std::string_view message) {
    co_await send(ex, make_response(ex, status, std::format("{}\n", message), text_plain));
}

awaitable<void> send_method_not_allowed(Exchange& ex, std::string_view allow, std::string_view message) {
    auto res = make_response(ex, http::status::method_not_allowed, std::format("{}\n", message), text_plain);
    res.set(http::field::allow, allow);
    co_await send(ex, std::move(res));
}

/// challenge: WWW-Authenticate makes the browser show its login dialog,
/// requests from the upload form's script don't want it
awaitable<void> send_unauthorized(Exchange& ex, const Share& share, bool password_given, bool challenge) {
    // a request without a password is the normal first step of a login, only wrong passwords are delayed
    if (password_given) {
        asio::steady_timer delay(ex.stream.get_executor(), auth_fail_delay);
        co_await delay.async_wait(use_awaitable);
    }
    auto res = make_response(ex, http::status::unauthorized,
        password_given ? "wrong password\n" : "password required\n", text_plain);
    if (challenge)
        res.set(http::field::www_authenticate, std::format("Basic realm=\"{}\", charset=\"UTF-8\"", share.url));
    co_await send(ex, std::move(res));
}

std::string http_date(fs::file_time_type t) {
    auto sys = std::chrono::floor<std::chrono::seconds>(std::chrono::file_clock::to_sys(t));
    return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", sys);
}

std::string human_size(u64 n) {
    if (n < 1024)
        return std::format("{} B", n);
    static constexpr std::string_view units[] = { "KiB", "MiB", "GiB", "TiB" };
    f64 v = static_cast<f64>(n);
    size_t unit = 0;
    for (v /= 1024; v >= 1024 && unit + 1 < std::size(units); v /= 1024)
        unit++;
    return std::format("{:.1f} {}", v, units[unit]);
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
        http::response<http::empty_body> res{ http::status::not_modified, ex.version };
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

    http::response<http::empty_body> res{
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
        ex.stream.expires_after(io_timeout);
        co_await asio::async_write(ex.stream, asio::buffer(chunk.get(), n), use_awaitable);
        remain -= n;
        ex.bytes += n;
    }
}

awaitable<void> send_share_list(Exchange& ex) {
    co_await send(ex, make_response(ex, http::status::ok, SharesPage{}.render(ex.config), text_html));
}

/// data for templates/ListingPage.htcpp
struct ListingEntry {
    std::string name;
    bool is_dir;
    std::string size;   // human-readable, empty for directories
    std::string date;   // last modification, UTC
};

struct Listing {
    std::string path;       // decoded URL of the directory, ends with '/'
    std::string parent;     // decoded URL of the parent directory
    bool writable;          // the share has write_password: show the upload form
    std::string free_space; // human-readable, empty if unknown
    std::vector<ListingEntry> entries;
};

awaitable<void> send_listing(Exchange& ex, const Share& share, std::string_view url_path, const fs::path& dir) {
    Listing listing;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = path_utf8(it->path().filename());
        if (is_temp_file(name))
            continue;
        std::error_code entry_ec;
        bool is_dir = it->is_directory(entry_ec);
        u64 size = is_dir ? 0 : it->file_size(entry_ec);
        auto mtime = std::chrono::floor<std::chrono::minutes>(std::chrono::file_clock::to_sys(it->last_write_time(entry_ec)));
        listing.entries.push_back({
            std::move(name), is_dir, is_dir ? "" : human_size(entry_ec ? 0 : size), std::format("{:%F %R}", mtime) });
    }
    if (ec) {
        co_await send_message(ex, http::status::internal_server_error, "can't read the directory");
        co_return;
    }
    // directories first, then by name
    std::ranges::sort(listing.entries, [](const ListingEntry& a, const ListingEntry& b) {
        return a.is_dir != b.is_dir ? a.is_dir : a.name < b.name;
    });

    listing.path = url_path;
    if (!listing.path.ends_with('/'))
        listing.path += '/';
    listing.parent = "/";
    if (listing.path != share.url + '/')
        listing.parent = listing.path.substr(0, listing.path.find_last_of('/', listing.path.size() - 2) + 1);
    listing.writable = share.write_password.has_value();
    fs::space_info space = fs::space(dir, ec);
    if (!ec)
        listing.free_space = human_size(space.available);

    co_await send(ex, make_response(ex, http::status::ok, ListingPage{}.render(listing), text_html));
}

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

/// nullopt if a file can be written to target, otherwise why not
std::optional<std::string_view> target_error(const fs::path& target) {
    std::error_code ec;
    if (fs::is_directory(target, ec))
        return "a directory with this name exists";
    if (!fs::is_directory(target.parent_path(), ec))
        return "the parent directory doesn't exist";
    // the file would be confused with an upload in progress
    if (is_temp_file(path_utf8(target.filename())))
        return "names ending with .uploading are reserved for uploads in progress";
    return std::nullopt;
}

constexpr std::string_view upload_in_progress = "the file is being uploaded by another client";

awaitable<void> send_too_large(Exchange& ex) {
    co_await send_message(ex, http::status::payload_too_large,
        std::format("the file is larger than the limit of {}", human_size(ex.config.max_upload)));
}

/// OPTIONS with X-Upload-Size: would a PUT of this size be accepted?
/// 200: the file exists and would be replaced, 404: it would be created, otherwise the error PUT would return.
awaitable<void> handle_upload_check(Exchange& ex, const fs::path& target, std::string_view size_str) {
    u64 size;
    auto [end, ec] = std::from_chars(size_str.data(), size_str.data() + size_str.size(), size);
    if (ec != std::errc() || end != size_str.data() + size_str.size())
        co_await send_message(ex, http::status::bad_request, "invalid X-Upload-Size");
    else if (size > ex.config.max_upload)
        co_await send_too_large(ex);
    else if (std::optional<std::string_view> error = target_error(target))
        co_await send_message(ex, http::status::conflict, *error);
    else if (is_uploading(target))
        co_await send_message(ex, http::status::conflict, upload_in_progress);
    else if (std::error_code exists_ec; fs::exists(target, exists_ec))
        co_await send_message(ex, http::status::ok, "file exists");
    else
        co_await send_message(ex, http::status::not_found, "file doesn't exist");
}

awaitable<void> handle_put(Exchange& ex, const fs::path& target) {
    boost::optional<u64> length = ex.parser.content_length();
    if (!length && !ex.parser.chunked()) {
        co_await send_message(ex, http::status::length_required, "Content-Length or chunked Transfer-Encoding required");
        co_return;
    }
    // checked before reading the body, the client doesn't send gigabytes for nothing
    if (length && *length > ex.config.max_upload) {
        co_await send_too_large(ex);
        co_return;
    }
    if (std::optional<std::string_view> error = target_error(target)) {
        co_await send_message(ex, http::status::conflict, *error);
        co_return;
    }
    // declared first, so it's released after the temp file is removed
    UploadClaim claim(target);
    if (!claim.claimed) {
        co_await send_message(ex, http::status::conflict, upload_in_progress);
        co_return;
    }
    std::error_code ec;
    bool existed = fs::exists(target, ec);

    // the client waits for this before sending the body
    if (beast::iequals(ex.parser.get()[http::field::expect], "100-continue")) {
        http::response<http::empty_body> res{ http::status::continue_, ex.version };
        ex.stream.expires_after(io_timeout);
        co_await http::async_write(ex.stream, res, use_awaitable);
    }

    // declared before the parser, so the parser closes the file before TempFile removes it
    TempFile tmp{ temp_path(target) };
    http::request_parser<http::file_body> parser(std::move(ex.parser));
    parser.body_limit(ex.config.max_upload);
    beast::error_code bec;
    // write, not write_new: a temp file left by a crashed server is overwritten, the claim says no upload uses it
    parser.get().body().open(path_utf8(tmp.path).c_str(), beast::file_mode::write, bec);
    if (bec) {
        log_error("can't create {}: {}", path_utf8(tmp.path), bec.message());
        co_await send_message(ex, http::status::internal_server_error, "can't create the file, see the server log");
        co_return;
    }

    // read in parts: the timeout applies to each part, not to the whole upload
    while (!parser.is_done()) {
        ex.stream.expires_after(io_timeout);
        auto [read_ec, n] = co_await http::async_read_some(ex.stream, ex.buffer, parser, asio::as_tuple(use_awaitable));
        if (read_ec == http::error::body_limit) {
            co_await send_too_large(ex);
            co_return;
        }
        if (read_ec)
            throw boost::system::system_error(read_ec, "upload");
    }
    ex.body_pending = false;
    parser.get().body().close();

    // atomic: other clients see either the old or the new file, never a partial one
    fs::rename(tmp.path, target, ec);
    if (ec) {
        log_error("can't rename {} to {}: {}", path_utf8(tmp.path), path_utf8(target), ec.message());
        co_await send_message(ex, http::status::internal_server_error, "can't save the file, see the server log");
        co_return;
    }
    tmp.keep = true;
    if (existed)
        co_await send(ex, make_response(ex, http::status::no_content, "", text_plain));  // 204 has no body
    else
        co_await send_message(ex, http::status::created, "file created");
}

awaitable<void> handle_request(Exchange& ex) {
    const auto& req = ex.parser.get();
    http::verb method = req.method();
    if (method != http::verb::get && method != http::verb::head
        && method != http::verb::put && method != http::verb::options) {
        co_await send_method_not_allowed(ex, "GET, HEAD, PUT, OPTIONS", "method not allowed");
        co_return;
    }

    std::string_view target = req.target();
    std::optional<std::string> path;
    if (target.starts_with('/'))
        path = percent_decode(target.substr(0, target.find('?')));
    if (!path) {
        co_await send_message(ex, http::status::bad_request, "invalid URL");
        co_return;
    }

    if (*path == "/") {
        if (method == http::verb::put || method == http::verb::options)
            co_await send_method_not_allowed(ex, "GET, HEAD", "uploads are possible only inside a share");
        else
            co_await send_share_list(ex);
        co_return;
    }

    std::string_view rest;
    const Share* share = ex.config.find_share(*path, rest);
    if (!share) {
        co_await send_message(ex, http::status::not_found, "not found");
        co_return;
    }

    // OPTIONS is used only by the upload form: would a PUT be accepted? Asked before sending the file.
    bool upload_check = method == http::verb::options;
    std::string_view upload_size = req["X-Upload-Size"];
    if (upload_check && upload_size.empty()) {
        co_await send_message(ex, http::status::bad_request, "X-Upload-Size header required");
        co_return;
    }

    Access access = method == http::verb::put || upload_check ? Access::write : Access::read;
    if (access == Access::write && !share->write_password) {
        co_await send_method_not_allowed(ex, "GET, HEAD", "uploads are disabled for this share");
        co_return;
    }
    std::string_view authorization = req[http::field::authorization];
    if (!is_authorized(*share, access, basic_auth_password(authorization))) {
        co_await send_unauthorized(ex, *share, !authorization.empty(), !upload_check);
        co_return;
    }

    std::optional<fs::path> file = resolve(share->path, rest);
    if (!file) {
        co_await send_message(ex, http::status::bad_request,
            "invalid path: names can't be \"..\", contain \\ : * ? \" < > | or control characters, "
            "end with a dot or space, or be a device name like CON");
        co_return;
    }
    if (upload_check)
        co_await handle_upload_check(ex, *file, upload_size);
    else if (access == Access::write)
        co_await handle_put(ex, *file);
    else
        co_await handle_get(ex, *share, *path, *file);
}

} // namespace

awaitable<void> http_session(tcp::socket socket, const Config& config) {
    boost::system::error_code ec;
    std::string peer = socket.remote_endpoint(ec).address().to_string();
    beast::tcp_stream stream(std::move(socket));
    beast::flat_buffer buffer;
    try {
        for (;;) {
            RequestParser parser;
            parser.header_limit(header_limit);
            // the default limit of 1 MiB is checked against Content-Length already when the header is read,
            // handle_put() checks max_upload itself
            parser.body_limit(boost::none);
            stream.expires_after(io_timeout);
            auto [read_ec, n] = co_await http::async_read_header(stream, buffer, parser, asio::as_tuple(use_awaitable));
            if (read_ec && is_disconnect(read_ec))
                break;

            Exchange ex{ stream, buffer, parser, config, parser.get().version(),
                parser.get().method() == http::verb::head, !parser.is_done() };
            // copied: PUT moves the request out of the parser
            std::string method(parser.get().method_string());
            std::string target(parser.get().target());
            std::string_view forwarded_for = parser.get()["X-Forwarded-For"];
            std::string client = forwarded_for.empty() ? peer : std::string(forwarded_for);
            auto start = std::chrono::steady_clock::now();

            if (read_ec) {
                // malformed request
                ex.version = 11;
                ex.body_pending = true;
                co_await send_message(ex, http::status::bad_request, "malformed request");
            }
            else {
                co_await handle_request(ex);
            }

            auto ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            log_info("{} {} {} {} {} {}us", client, method, target, static_cast<unsigned>(ex.status), human_size(ex.bytes), ms.count());
            if (!ex.keep_alive)
                break;
        }
        // the stream's destructor closes the connection
    }
    catch (const boost::system::system_error& e) {
        if (!is_disconnect(e.code()))
            throw;
    }
}
