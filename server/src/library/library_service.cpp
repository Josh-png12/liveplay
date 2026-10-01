#include "liveplay/library/library_service.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <stdexcept>

namespace liveplay::library {
TrackPage LibraryService::list(std::size_t offset, std::size_t limit) const {
    if (limit == 0 || limit > 500) throw std::invalid_argument("limit must be 1..500");
    auto page = repository_.list(offset, limit);
    for (auto& track : page.tracks) {
        if (!std::filesystem::is_regular_file(util::utf8_to_path(track.asset_path))) track.status = "missing";
    }
    return page;
}
std::optional<Track> LibraryService::find(const std::string& id) const {
    auto track = repository_.find(id);
    if (track && !std::filesystem::is_regular_file(util::utf8_to_path(track->asset_path))) track->status = "missing";
    return track;
}
std::optional<Track> LibraryService::find_by_hash(const std::string& hash) const {
    return repository_.find_by_hash(hash);
}
TrackInsert LibraryService::register_track(const Track& track) {
    return repository_.insert(track);
}
} // namespace liveplay::library
