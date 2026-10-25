#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "Config.hpp"

/// Serves HTTP/1.1 requests on the connection until the client closes it or keep-alive ends.
/// Client disconnects and timeouts are not errors, other errors are thrown.
boost::asio::awaitable<void> http_session(boost::asio::ip::tcp::socket socket, const Config& config);
