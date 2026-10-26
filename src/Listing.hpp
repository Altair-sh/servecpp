#pragma once

#include <filesystem>
#include <string_view>
#include <boost/asio/awaitable.hpp>
#include "Config.hpp"
#include "Exchange.hpp"

// HTML pages, rendered from templates/*.htcpp

/// page of GET /: links to all shares
boost::asio::awaitable<void> send_share_list(Exchange& ex);

/// page of a directory: files, subdirectories and the upload form if the share is writable
/// url_path: decoded URL of dir
boost::asio::awaitable<void> send_listing(Exchange& ex, const Share& share, std::string_view url_path, const std::filesystem::path& dir);
