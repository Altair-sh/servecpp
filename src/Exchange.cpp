#include "Exchange.hpp"

#include <array>
#include <format>
#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using asio::awaitable;
using asio::use_awaitable;

namespace {

void set_common_headers(Exchange& ex, Response& res) {
    res.set(http::field::server, "servecpp");
    if (ex.body_pending)
        ex.keep_alive = false;
    res.keep_alive(ex.keep_alive);
    ex.status = res.result();
}

/// status line and fields, written here instead of by Beast's serializer, which is slow to compile
std::string serialize_header(const Response& res) {
    std::string out = std::format("HTTP/{}.{} {} {}\r\n",
        res.version() / 10, res.version() % 10, res.result_int(), std::string_view(res.reason()));
    for (const auto& field : res)
        out += std::format("{}: {}\r\n", std::string_view(field.name_string()), std::string_view(field.value()));
    out += "\r\n";
    return out;
}

/// The only write of the server, all responses go through it.
/// Header and body in one write: a small body written separately can wait for the client's delayed ACK.
awaitable<void> write(Exchange& ex, std::string_view header, std::string_view body) {
    std::array<asio::const_buffer, 2> buffers{ asio::buffer(header), asio::buffer(body) };
    ex.stream.expires_after(io_timeout);
    co_await asio::async_write(ex.stream, buffers, use_awaitable);
}

} // namespace

awaitable<boost::system::error_code> read_some(beast::tcp_stream& stream, beast::flat_buffer& buffer, RequestParser& parser) {
    auto [ec, n] = co_await http::async_read_some(stream, buffer, parser, asio::as_tuple(use_awaitable));
    // the body buffer is full, not an error
    if (ec == http::error::need_buffer)
        ec = {};
    co_return ec;
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

Response make_response(const Exchange& ex, http::status status, std::string body, std::string_view type) {
    Response res{status, ex.version};
    res.set(http::field::content_type, type);
    res.body() = std::move(body);
    return res;
}

awaitable<void> send(Exchange& ex, Response res) {
    set_common_headers(ex, res);
    // 204 and 304 have no body and no Content-Length
    if (res.result() != http::status::no_content && res.result() != http::status::not_modified)
        res.content_length(res.body().size());
    std::string header = serialize_header(res);
    std::string_view body = ex.head ? std::string_view() : std::string_view(res.body());
    ex.bytes = body.size();
    co_await write(ex, header, body);
}

awaitable<void> send_header(Exchange& ex, Response& res) {
    set_common_headers(ex, res);
    std::string header = serialize_header(res);
    co_await write(ex, header, {});
}

awaitable<void> send_body(Exchange& ex, std::string_view data) {
    co_await write(ex, {}, data);
    ex.bytes += data.size();
}

awaitable<void> send_continue(Exchange& ex) {
    co_await write(ex, "HTTP/1.1 100 Continue\r\n\r\n", {});
}

awaitable<void> send_message(Exchange& ex, http::status status, std::string_view message) {
    co_await send(ex, make_response(ex, status, std::format("{}\n", message), text_plain));
}
