#include "Upload.hpp"

#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "Logger.hpp"
#include "Path.hpp"
#include "TempFile.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace fs = std::filesystem;
using asio::awaitable;
using asio::use_awaitable;

namespace {

constexpr std::string_view upload_in_progress = "the file is being uploaded by another client";

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

awaitable<void> send_too_large(Exchange& ex) {
    co_await send_message(ex, http::status::payload_too_large,
        std::format("the file is larger than the limit of {}", human_size(ex.config.max_upload)));
}

} // namespace

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
    if (beast::iequals(ex.parser.get()[http::field::expect], "100-continue"))
        co_await send_continue(ex);

    // declared before the file, so the file is closed before TempFile removes it
    TempFile tmp{ temp_path(target) };
    beast::file file;
    beast::error_code bec;
    // write, not write_new: a temp file left by a crashed server is overwritten, the claim says no upload uses it
    file.open(path_utf8(tmp.path).c_str(), beast::file_mode::write, bec);
    if (bec) {
        log_error("can't create {}: {}", path_utf8(tmp.path), bec.message());
        co_await send_message(ex, http::status::internal_server_error, "can't create the file, see the server log");
        co_return;
    }

    // chunked bodies have no Content-Length, the parser checks the limit while reading
    ex.parser.body_limit(ex.config.max_upload);
    auto chunk = std::make_unique_for_overwrite<char[]>(file_chunk_size);
    auto& body = ex.parser.get().body();
    // read in parts: the timeout applies to each part, not to the whole upload
    while (!ex.parser.is_done()) {
        body.data = chunk.get();
        body.size = file_chunk_size;
        ex.stream.expires_after(io_timeout);
        boost::system::error_code read_ec = co_await read_some(ex.stream, ex.buffer, ex.parser);
        if (read_ec == http::error::body_limit) {
            co_await send_too_large(ex);
            co_return;
        }
        if (read_ec)
            throw boost::system::system_error(read_ec, "upload");
        // size is what is left of the buffer
        file.write(chunk.get(), file_chunk_size - body.size, bec);
        if (bec) {
            log_error("can't write {}: {}", path_utf8(tmp.path), bec.message());
            co_await send_message(ex, http::status::internal_server_error, "can't write the file, see the server log");
            co_return;
        }
    }
    ex.body_pending = false;
    file.close(bec);

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
