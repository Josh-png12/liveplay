#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>

namespace liveplay::util {

// Caller serializes writes to a given path. Never delete the old destination
// before replacing it; Windows requires an explicit replace-existing operation.
void atomic_write_json(const std::filesystem::path& path, const nlohmann::json& value);
void atomic_write_text(const std::filesystem::path& path, std::string_view value);
void append_durable_line(const std::filesystem::path& path, std::string_view value);
nlohmann::json read_bounded_json(const std::filesystem::path& path,
                               std::uintmax_t max_bytes);

} // namespace liveplay::util
