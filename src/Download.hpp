#pragma once

#include <filesystem>
#include <string_view>
#include <boost/asio/awaitable.hpp>
#include "Config.hpp"
#include "Exchange.hpp"

/// GET/HEAD: a file (with Range and If-Modified-Since) or the listing of a directory
/// url_path: decoded URL, file: its resolved path in the share
boost::asio::awaitable<void> handle_get(Exchange& ex, const Share& share, std::string_view url_path, const std::filesystem::path& file);
