#pragma once

#include "liveplay/library/track.hpp"
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace liveplay::library {

struct TrackPage {
    std::vector<Track> tracks;
    std::size_t total = 0;
};
struct TrackInsert {
    Track track;
    bool inserted = false;
};

// Atomic uniqueness by content hash is part of the contract, including concurrent
// writers. A failed write must not publish a new track in memory or on disk.
class TrackRepository {
public:
    virtual ~TrackRepository() = default;
    virtual TrackPage list(std::size_t offset, std::size_t limit) const = 0;
    virtual std::optional<Track> find(const std::string& id) const = 0;
    virtual std::optional<Track> find_by_hash(const std::string& hash) const = 0;
    virtual TrackInsert insert(const Track& track) = 0;
    virtual void update_asset(const Track& track) = 0;
};

// Initial bounded JSON implementation. SQLite can replace this without changing
// LibraryService, ImportService or their HTTP contract. One server owns the store.
class JsonTrackRepository final : public TrackRepository {
public:
    static constexpr std::size_t max_tracks = 10'000;
    explicit JsonTrackRepository(std::filesystem::path path, std::filesystem::path portable_root = {});
    TrackPage list(std::size_t offset, std::size_t limit) const override;
    std::optional<Track> find(const std::string& id) const override;
    std::optional<Track> find_by_hash(const std::string& hash) const override;
    TrackInsert insert(const Track& track) override;
    void update_asset(const Track& track) override;
private:
    std::filesystem::path path_;
    std::filesystem::path portable_root_;
    mutable std::mutex mutex_;
    std::map<std::string, Track> tracks_;
    std::map<std::string, std::string> hashes_;
    void commit(const std::map<std::string, Track>& next);
};

} // namespace liveplay::library
