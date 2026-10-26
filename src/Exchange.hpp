#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <boost/asio/awaitable.hpp>
#include <boost/beast.hpp>
#include "Config.hpp"
#include "std.hpp"

// State of one request and functions to send its response, shared by the request handlers.

inline constexpr std::chrono::seconds io_timeout{ 30 };    // idle keep-alive, request header, each body chunk
inline constexpr size_t file_chunk_size = 256 * 1024;       // download and upload
inline constexpr std::string_view text_plain = "text/plain; charset=utf-8";
inline constexpr std::string_view text_html = "text/html; charset=utf-8";

/// the only request parser type, for the same reason as Response: the body is read into the caller's buffer
using RequestParser = boost::beast::http::request_parser<boost::beast::http::buffer_body>;
/// the only response type: Beast's write code is compiled for each body type, so all responses use one
using Response = boost::beast::http::response<boost::beast::http::string_body>;

/// One request and its response. Handlers fill status, bytes and keep_alive.
struct Exchange {
    boost::beast::tcp_stream& stream;
    boost::beast::flat_buffer& buffer;
    RequestParser& parser;
    const Config& config;
    u32 version;
    bool head;
    bool body_pending;              // request body not read yet: the connection can't be reused
    boost::beast::http::status status = boost::beast::http::status::ok;
    u64 bytes = 0;                  // response body bytes sent
    bool keep_alive = true;
};

/// The only read of the server: reads and parses the next part of the request, the header or a body part.
/// The body part goes to parser.get().body().data, the caller sets data and size before the call.
/// The caller sets the timeout.
boost::asio::awaitable<boost::system::error_code> read_some(
    boost::beast::tcp_stream& stream, boost::beast::flat_buffer& buffer, RequestParser& parser);

/// "512 B", "1.5 MiB"
std::string human_size(u64 n);

Response make_response(const Exchange& ex, boost::beast::http::status status, std::string body, std::string_view type);

/// sends the whole response, HEAD gets the same Content-Length, but no body
boost::asio::awaitable<void> send(Exchange& ex, Response res);

/// sends only the header with the Content-Length set by the caller, the body follows with send_body()
boost::asio::awaitable<void> send_header(Exchange& ex, Response& res);

/// part of the body after send_header()
boost::asio::awaitable<void> send_body(Exchange& ex, std::string_view data);

/// "100 Continue": the client waits for it before sending the request body
boost::asio::awaitable<void> send_continue(Exchange& ex);

/// The status says what happened, the body says why, readable by people (curl) and the upload form.
/// The reason phrase isn't used for that: HTTP/2 between the browser and the proxy doesn't have it.
boost::asio::awaitable<void> send_message(Exchange& ex, boost::beast::http::status status, std::string_view message);
