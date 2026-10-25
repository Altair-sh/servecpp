#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "Config.hpp"

enum class Access { read, write };

/// password from "Authorization: Basic base64(user:password)", the user name is ignored.
/// nullopt if the header is missing or malformed.
std::optional<std::string> basic_auth_password(std::string_view authorization);

/// Read: no read_password, or read_password or write_password matches.
/// Write: write_password matches. A share without write_password is read-only, check it before.
bool is_authorized(const Share& share, Access access, const std::optional<std::string>& password);
