#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <boost/asio/ip/address.hpp>
#include "std.hpp"

/// directory available under an URL prefix
struct Share {
    std::string url;                // "/public", no trailing '/'
    std::filesystem::path path;     // canonical directory on disk
    std::optional<std::string> read_password;   // nullopt: anyone can read
    std::optional<std::string> write_password;  // nullopt: read-only
};

struct Config {
    boost::asio::ip::address address;
    u16 port;
    u64 max_upload;
    std::vector<Share> shares;

    /// Share with the longest url that is a prefix of path at a segment boundary
    /// ("/public" matches "/public/a.txt", not "/publicity").
    /// rest receives the part of path after the url ("/a.txt" or "").
    const Share* find_share(std::string_view path, std::string_view& rest) const;
};

/// Reads the TOML config. Relative share paths are relative to the config file directory.
/// Throws std::runtime_error with a message that names the file and the problem.
Config load_config(const std::filesystem::path& file);
