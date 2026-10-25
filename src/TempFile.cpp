#include "TempFile.hpp"

#include <mutex>
#include <set>
#include "Logger.hpp"
#include "Path.hpp"

namespace fs = std::filesystem;

static constexpr std::string_view temp_suffix = ".uploading";

bool is_temp_file(std::string_view name) {
    return name.size() > temp_suffix.size() && name.ends_with(temp_suffix);
}

fs::path temp_path(const fs::path& target) {
    fs::path tmp = target;
    tmp += temp_suffix;
    return tmp;
}

TempFile::~TempFile() {
    std::error_code ec;
    if (!keep)
        fs::remove(path, ec);
}

// targets with an upload in progress, shared by all connections
static std::mutex uploads_mutex;
static std::set<fs::path> uploads;

bool is_uploading(const fs::path& target) {
    std::lock_guard lock(uploads_mutex);
    return uploads.contains(target);
}

UploadClaim::UploadClaim(fs::path target) : target(std::move(target)) {
    std::lock_guard lock(uploads_mutex);
    claimed = uploads.insert(this->target).second;
}

UploadClaim::~UploadClaim() {
    std::lock_guard lock(uploads_mutex);
    if (claimed)
        uploads.erase(target);
}

void remove_unfinished_uploads(const Config& config) {
    for (const Share& share : config.shares) {
        std::error_code ec;
        fs::recursive_directory_iterator it(share.path, fs::directory_options::skip_permission_denied, ec);
        for (fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
            std::error_code entry_ec;
            if (!it->is_regular_file(entry_ec) || !is_temp_file(path_utf8(it->path().filename())))
                continue;
            if (fs::remove(it->path(), entry_ec))
                log_warning("removed unfinished upload {}", path_utf8(it->path()));
            else
                log_warning("can't remove unfinished upload {}: {}", path_utf8(it->path()), entry_ec.message());
        }
        if (ec)
            log_warning("can't search {} for unfinished uploads: {}", path_utf8(share.path), ec.message());
    }
}
