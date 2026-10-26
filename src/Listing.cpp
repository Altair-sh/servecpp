#include "Listing.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <vector>
#include "Path.hpp"
#include "TempFile.hpp"
// generated from templates/*.htcpp
#include "ListingPage.h"
#include "SharesPage.h"

namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace fs = std::filesystem;
using asio::awaitable;

namespace {

/// data for templates/ListingPage.htcpp
struct ListingEntry {
    std::string name;
    bool is_dir;
    std::string size;   // human-readable, empty for directories
    std::string date;   // last modification, UTC
};

struct Listing {
    std::string path;       // decoded URL of the directory, ends with '/'
    std::string parent;     // decoded URL of the parent directory
    bool writable;          // the share has write_password: show the upload form
    std::string free_space; // human-readable, empty if unknown
    std::vector<ListingEntry> entries;
};

} // namespace

awaitable<void> send_share_list(Exchange& ex) {
    co_await send(ex, make_response(ex, http::status::ok, SharesPage{}.render(ex.config), text_html));
}

awaitable<void> send_listing(Exchange& ex, const Share& share, std::string_view url_path, const fs::path& dir) {
    Listing listing;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = path_utf8(it->path().filename());
        if (is_temp_file(name))
            continue;
        std::error_code entry_ec;
        bool is_dir = it->is_directory(entry_ec);
        u64 size = is_dir ? 0 : it->file_size(entry_ec);
        auto mtime = std::chrono::floor<std::chrono::minutes>(std::chrono::file_clock::to_sys(it->last_write_time(entry_ec)));
        listing.entries.push_back({
            std::move(name), is_dir, is_dir ? "" : human_size(entry_ec ? 0 : size), std::format("{:%F %R}", mtime) });
    }
    if (ec) {
        co_await send_message(ex, http::status::internal_server_error, "can't read the directory");
        co_return;
    }
    // directories first, then by name
    std::ranges::sort(listing.entries, [](const ListingEntry& a, const ListingEntry& b) {
        return a.is_dir != b.is_dir ? a.is_dir : a.name < b.name;
    });

    listing.path = url_path;
    if (!listing.path.ends_with('/'))
        listing.path += '/';
    listing.parent = "/";
    if (listing.path != share.url + '/')
        listing.parent = listing.path.substr(0, listing.path.find_last_of('/', listing.path.size() - 2) + 1);
    listing.writable = share.write_password.has_value();
    fs::space_info space = fs::space(dir, ec);
    if (!ec)
        listing.free_space = human_size(space.available);

    co_await send(ex, make_response(ex, http::status::ok, ListingPage{}.render(listing), text_html));
}
