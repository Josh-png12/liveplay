#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace liveplay::library {

// Library identity is independent of project UUIDs and engine CueIds.
struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::int64_t duration_ms = 0;
    std::string format; // Source container extension; not a codec claim.
    std::uint32_t sample_rate = 0; // 0 = unknown.
    std::uint32_t channels = 0;
    std::string asset_path; // Absolute at API boundary; relative in portable catalogs.
    std::string status = "available"; // available, missing, corrupt
    std::string content_hash; // sha256:<hex>, exact bytes, not a fingerprint.
    std::uint64_t size_bytes = 0;
    std::string import_origin; // server_path or upload (first import wins).
    std::string source_name;
    std::string imported_at;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Track, id, title, artist, album, duration_ms,
    format, sample_rate, channels, asset_path, content_hash, size_bytes,
    import_origin, source_name, imported_at, status)

} // namespace liveplay::library
