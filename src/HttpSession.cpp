#include "HttpSession.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "Auth.hpp"
#include "Download.hpp"
#include "Exchange.hpp"
#include "Listing.hpp"
#include "Logger.hpp"
#include "Path.hpp"
#include "Upload.hpp"
#include "std.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using asio::awaitable;
using asio::use_awaitable;
using asio::ip::tcp;

namespace {

constexpr auto auth_fail_delay = 1s;    // slows down password guessing
constexpr size_t header_limit = 8 * 1024;

bool is_disconnect(const boost::system::error_code& ec) {
    return ec == http::error::end_of_stream || ec == http::error::partial_message
        || ec == beast::error::timeout || ec == asio::error::eof
        || ec == asio::error::connection_reset || ec == asio::error::connection_aborted
        || ec == asio::error::broken_pipe || ec == asio::error::operation_aborted;
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
            // for the whole header, not for each part
            stream.expires_after(io_timeout);
            boost::system::error_code read_ec;
            while (!read_ec && !parser.is_header_done())
                read_ec = co_await read_some(stream, buffer, parser);
            if (read_ec && is_disconnect(read_ec))
                break;

            Exchange ex{ stream, buffer, parser, config, parser.get().version(),
                parser.get().method() == http::verb::head, !parser.is_done() };
            std::string_view method = parser.get().method_string();
            std::string_view target = parser.get().target();
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
            log_info("{} {} {} {} {} {}us", client, method, target, static_cast<u32>(ex.status), human_size(ex.bytes), ms.count());
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
