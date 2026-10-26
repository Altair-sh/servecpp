#pragma once

#include <filesystem>
#include <string_view>

/// Content-Type by file extension, application/octet-stream if unknown.
/// html, svg, xml and js are deliberately missing: uploaded files must not run scripts on this site.
std::string_view mime_type(const std::filesystem::path& p);
