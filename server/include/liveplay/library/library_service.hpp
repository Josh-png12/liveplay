#pragma once

#include "liveplay/library/track_repository.hpp"

namespace liveplay::library {

// No ProjectState/AudioEngine dependency. An analysis scheduler can consume track
// IDs + content_hash + asset_path later without creating a playback cue.
class LibraryService {
public:
    explicit LibraryService(TrackRepository& repository) : repository_(repository) {}
    TrackPage list(std::size_t offset = 0, std::size_t limit = 100) const;
    std::optional<Track> find(const std::string& id) const;
    std::optional<Track> find_by_hash(const std::string& hash) const;
    TrackInsert register_track(const Track& track);
    void update_asset(const Track& track) { repository_.update_asset(track); }
    void checkpoint() { repository_.checkpoint(); }
    std::mutex& asset_mutex() { return asset_mutex_; }
private:
    TrackRepository& repository_;
    std::mutex asset_mutex_;
};

} // namespace liveplay::library
