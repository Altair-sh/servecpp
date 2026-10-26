#pragma once

#include <filesystem>
#include <string_view>
#include <boost/asio/awaitable.hpp>
#include "Exchange.hpp"

/// OPTIONS with X-Upload-Size: would a PUT of this size be accepted?
/// 200: the file exists and would be replaced, 404: it would be created, otherwise the error PUT would return.
boost::asio::awaitable<void> handle_upload_check(Exchange& ex, const std::filesystem::path& target, std::string_view size_str);

/// PUT: writes the body to a temp file and renames it to target
boost::asio::awaitable<void> handle_put(Exchange& ex, const std::filesystem::path& target);
