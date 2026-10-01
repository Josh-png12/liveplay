#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace liveplay::util {

// Caller serializes writes to a given path. Never delete the old destination
// before replacing it; Windows requires an explicit replace-existing operation.
void atomic_write_json(const std::filesystem::path& path, const nlohmann::json& value);
nlohmann::json read_bounded_json(const std::filesystem::path& path,
                               std::uintmax_t max_bytes);

} // namespace liveplay::util
