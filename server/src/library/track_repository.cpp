#include "liveplay/library/track_repository.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <algorithm>
#include <stdexcept>

namespace liveplay::library {
namespace {
constexpr std::uintmax_t max_catalog_bytes = 64ull * 1024 * 1024;
void validate(const Track& t) {
    const auto hash = t.content_hash;
    if (hash.size() != 71 || !hash.starts_with("sha256:") ||
        !std::all_of(hash.begin() + 7, hash.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }) || t.id != "track_" + hash.substr(7) || t.title.empty() ||
        t.duration_ms <= 0 || t.size_bytes == 0 ||
        !util::utf8_to_path(t.asset_path).is_absolute() ||
        (t.import_origin != "server_path" && t.import_origin != "upload") ||
        (t.status != "available" && t.status != "missing" && t.status != "corrupt")) {
        throw std::invalid_argument("invalid track record");
    }
}
}

JsonTrackRepository::JsonTrackRepository(std::filesystem::path path, std::filesystem::path portable_root)
    : path_(std::move(path)), portable_root_(std::move(portable_root)) {
    if (!std::filesystem::exists(path_)) return;
    const auto doc = util::read_bounded_json(path_, max_catalog_bytes);
    if (!doc.is_object() || doc.at("schemaVersion") != 1 ||
        !doc.at("tracks").is_array() || doc.at("tracks").size() > max_tracks)
        throw std::runtime_error("unsupported or invalid library catalog");
    for (const auto& value : doc.at("tracks")) {
        auto track = value.get<Track>();
        const auto asset = util::utf8_to_path(track.asset_path);
        if (asset.is_relative() && !portable_root_.empty()) {
            const auto relative = asset.lexically_normal();
            if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("invalid relative asset path");
            track.asset_path = util::path_to_utf8(std::filesystem::absolute(portable_root_ / relative));
        }
        validate(track);
        if (!tracks_.emplace(track.id, track).second ||
            !hashes_.emplace(track.content_hash, track.id).second)
            throw std::runtime_error("duplicate identity in library catalog");
    }
}

TrackPage JsonTrackRepository::list(std::size_t offset, std::size_t limit) const {
    std::lock_guard lock(mutex_);
    TrackPage page;
    page.total = tracks_.size();
    auto it = tracks_.begin();
    std::advance(it, std::min(offset, tracks_.size()));
    while (it != tracks_.end() && page.tracks.size() < limit) {
        page.tracks.push_back(it->second);
        ++it;
    }
    return page;
}
std::optional<Track> JsonTrackRepository::find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = tracks_.find(id);
    return it == tracks_.end() ? std::nullopt : std::optional<Track>(it->second);
}
std::optional<Track> JsonTrackRepository::find_by_hash(const std::string& hash) const {
    std::lock_guard lock(mutex_);
    const auto it = hashes_.find(hash);
    return it == hashes_.end() ? std::nullopt : std::optional<Track>(tracks_.at(it->second));
}
TrackInsert JsonTrackRepository::insert(const Track& track) {
    validate(track);
    std::lock_guard lock(mutex_);
    if (const auto it = hashes_.find(track.content_hash); it != hashes_.end())
        return {tracks_.at(it->second), false};
    if (tracks_.size() >= max_tracks) throw std::runtime_error("library track limit reached");
    auto next = tracks_;
    next.emplace(track.id, track);
    commit(next);
    tracks_.swap(next);
    hashes_.emplace(track.content_hash, track.id);
    return {track, true};
}
void JsonTrackRepository::commit(const std::map<std::string, Track>& next) {
    auto values = nlohmann::json::array();
    for (const auto& [id, value] : next) {
        auto stored = value;
        if (!portable_root_.empty()) {
            const auto relative = util::utf8_to_path(value.asset_path).lexically_relative(portable_root_);
            if (!relative.empty() && *relative.begin() != "..") {
                const auto portable = relative.generic_u8string();
                stored.asset_path = std::string(reinterpret_cast<const char*>(portable.data()), portable.size());
            }
        }
        values.push_back(stored);
    }
    const nlohmann::json doc{{"schemaVersion", 1}, {"tracks", values}};
    if (doc.dump(2).size() > max_catalog_bytes)
        throw std::runtime_error("library catalog size limit reached");
    if (std::filesystem::exists(path_)) std::filesystem::copy_file(path_, path_.parent_path() / "catalog.backup.json",
        std::filesystem::copy_options::overwrite_existing);
    util::atomic_write_json(path_, doc); // Commit before publishing memory.
}
void JsonTrackRepository::update_asset(const Track& track) {
    validate(track);
    std::lock_guard lock(mutex_);
    const auto old = tracks_.find(track.id);
    if (old == tracks_.end() || old->second.content_hash != track.content_hash)
        throw std::invalid_argument("asset update cannot change identity");
    if (track.status != "available" && track.status != "missing" && track.status != "corrupt")
        throw std::invalid_argument("invalid asset status");
    auto next = tracks_; next.at(track.id) = track;
    commit(next); tracks_.swap(next);
}

} // namespace liveplay::library
