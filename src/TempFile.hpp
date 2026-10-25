#pragma once

#include <filesystem>
#include <string_view>
#include "Config.hpp"

// An upload is written to "<name>.uploading" next to the target
// and renamed to <name> when it completes.

/// "<name>.uploading": temp file of an upload in progress
bool is_temp_file(std::string_view name);

/// temp file for an upload to target
std::filesystem::path temp_path(const std::filesystem::path& target);

/// removes the file on destruction unless keep is set
struct TempFile {
    std::filesystem::path path;
    bool keep = false;

    ~TempFile();
};

/// an upload to target is in progress
bool is_uploading(const std::filesystem::path& target);

/// Claims target for one upload until destruction, claimed is false if another upload has it.
/// All uploads to a target use the same temp file, so only one can run at a time.
struct UploadClaim {
    std::filesystem::path target;
    bool claimed;

    explicit UploadClaim(std::filesystem::path target);
    ~UploadClaim();
    UploadClaim(const UploadClaim&) = delete;
    UploadClaim& operator=(const UploadClaim&) = delete;
};

/// Deletes temp files of uploads interrupted by a crash (*.uploading in all shares, recursively).
/// Call before accepting connections, when no upload can be running.
void remove_unfinished_uploads(const Config& config);
