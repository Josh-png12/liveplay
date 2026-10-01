#pragma once
#include <filesystem>
#include <string>
#include <functional>
namespace liveplay::library {
using Environment = std::function<std::string(const char*)>;
std::filesystem::path resolve_plus_directory(const std::string& override_path = {},
    Environment environment = {});
// Copy migration: never deletes the old store or overwrites an existing catalog.
bool migrate_plus_directory(const std::filesystem::path& legacy,
                            const std::filesystem::path& destination);
void backup_corrupt_catalog(const std::filesystem::path& catalog);
}
