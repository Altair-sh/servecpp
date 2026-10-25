#include "Config.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <toml++/toml.hpp>
#include "Path.hpp"

namespace fs = std::filesystem;
namespace asio = boost::asio;

/// file: config name for messages, node: where the problem is (adds the line number)
[[noreturn]] static void fail(std::string_view file, const toml::node* node, std::string_view msg) {
    if (node)
        throw std::runtime_error(std::format("{}:{}: {}", file, node->source().begin.line, msg));
    throw std::runtime_error(std::format("{}: {}", file, msg));
}

/// a typo like "read_pasword" must not silently make a share public
static void check_keys(std::string_view file, const toml::table& t, std::initializer_list<std::string_view> allowed) {
    for (auto&& [key, node] : t)
        if (std::ranges::find(allowed, key.str()) == allowed.end())
            fail(file, &node, std::format("unknown key '{}'", key.str()));
}

static std::optional<std::string> optional_string(std::string_view file, const toml::table& t, std::string_view key) {
    const toml::node* n = t.get(key);
    if (!n)
        return std::nullopt;
    if (!n->is_string())
        fail(file, n, std::format("'{}' must be a string", key));
    return n->as_string()->get();
}

static std::optional<i64> optional_integer(std::string_view file, const toml::table& t, std::string_view key) {
    const toml::node* n = t.get(key);
    if (!n)
        return std::nullopt;
    if (!n->is_integer())
        fail(file, n, std::format("'{}' must be an integer", key));
    return n->as_integer()->get();
}

static std::optional<std::string> optional_password(std::string_view file, const toml::table& t, std::string_view key) {
    std::optional<std::string> password = optional_string(file, t, key);
    if (password && password->empty())
        fail(file, t.get(key), std::format("'{}' is empty, remove the key to disable the password", key));
    return password;
}

static bool is_valid_url(std::string_view url) {
    // url is also used as the realm in WWW-Authenticate: realm="<url>"
    bool has_special = std::ranges::any_of(url, [](char c) {
        return static_cast<u8>(c) < 0x20 || std::string_view("?#\"\\").find(c) != std::string_view::npos;
    });
    return url.size() > 1 && url.starts_with('/') && !url.ends_with('/') && url.find("//") == std::string_view::npos
        && !has_special && is_valid_utf8(url);
}

Config load_config(const fs::path& file) {
    std::string name = path_utf8(file);
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error(std::format("{}: can't open file", name));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    toml::table root;
    try {
        root = toml::parse(text, name);
    }
    catch (const toml::parse_error& e) {
        throw std::runtime_error(std::format("{}:{}:{}: {}",
            name, e.source().begin.line, e.source().begin.column, e.description()));
    }
    check_keys(name, root, { "address", "port", "max_upload", "share" });

    Config config;

    std::string address = optional_string(name, root, "address").value_or("127.0.0.1");
    boost::system::error_code ec;
    config.address = asio::ip::make_address(address, ec);
    if (ec)
        fail(name, root.get("address"), std::format("invalid address '{}'", address));

    i64 port = optional_integer(name, root, "port").value_or(8080);
    if (port < 1 || port > 65535)
        fail(name, root.get("port"), "'port' must be in range 1-65535");
    config.port = static_cast<u16>(port);

    i64 max_upload = optional_integer(name, root, "max_upload").value_or(i64(4) << 30);
    if (max_upload <= 0)
        fail(name, root.get("max_upload"), "'max_upload' must be > 0");
    config.max_upload = static_cast<u64>(max_upload);

    const toml::node* shares = root.get("share");
    const toml::array* share_array = shares ? shares->as_array() : nullptr;
    if (!share_array || share_array->empty())
        fail(name, shares, "no [[share]] defined");

    for (const toml::node& node : *share_array) {
        const toml::table* t = node.as_table();
        if (!t)
            fail(name, &node, "'share' must be a table, use [[share]]");
        check_keys(name, *t, { "url", "path", "read_password", "write_password" });
        Share share;

        std::optional<std::string> url = optional_string(name, *t, "url");
        if (!url)
            fail(name, t, "share without 'url'");
        if (!is_valid_url(*url))
            fail(name, t->get("url"), std::format("invalid url '{}', expected '/name' without trailing '/'", *url));
        for (const Share& other : config.shares)
            if (other.url == *url)
                fail(name, t->get("url"), std::format("duplicate url '{}'", *url));
        share.url = std::move(*url);

        std::optional<std::string> path_str = optional_string(name, *t, "path");
        if (!path_str)
            fail(name, t, "share without 'path'");
        fs::path path = utf8_path(*path_str);
        if (path.is_relative())
            path = file.parent_path() / path;
        std::error_code fs_ec;
        if (!fs::is_directory(path, fs_ec))
            fail(name, t->get("path"), std::format("'path': '{}' is not a directory", *path_str));
        share.path = fs::canonical(path, fs_ec);
        if (fs_ec)
            fail(name, t->get("path"), std::format("'path': '{}': {}", *path_str, fs_ec.message()));

        share.read_password = optional_password(name, *t, "read_password");
        share.write_password = optional_password(name, *t, "write_password");
        config.shares.push_back(std::move(share));
    }
    return config;
}

const Share* Config::find_share(std::string_view path, std::string_view& rest) const {
    const Share* best = nullptr;
    for (const Share& share : shares) {
        if (!path.starts_with(share.url))
            continue;
        if (path.size() > share.url.size() && path[share.url.size()] != '/')
            continue;
        if (!best || share.url.size() > best->url.size())
            best = &share;
    }
    if (best)
        rest = path.substr(best->url.size());
    return best;
}
