#pragma once

#include "liveplay/library/track.hpp"
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

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
    // Metadata edits keep Track identity and content integrity immutable.
    virtual void update_metadata(const Track& track) { update_asset(track); }
    // Flushes a replaceable repository's compact representation. Mutations are
    // already durable when their methods return; this is a maintenance hint.
    virtual void checkpoint() {}
};

// Initial bounded JSON implementation. SQLite can replace this without changing
// LibraryService, ImportService or their HTTP contract. One server owns the store.
class JsonTrackRepository final : public TrackRepository {
public:
    static constexpr std::size_t max_tracks = 10'000;
    explicit JsonTrackRepository(std::filesystem::path path, std::filesystem::path portable_root = {},
                                 std::function<bool()> defer_compaction = {});
    ~JsonTrackRepository() override;
    TrackPage list(std::size_t offset, std::size_t limit) const override;
    std::optional<Track> find(const std::string& id) const override;
    std::optional<Track> find_by_hash(const std::string& hash) const override;
    TrackInsert insert(const Track& track) override;
    void update_asset(const Track& track) override;
    void update_metadata(const Track& track) override;
    void checkpoint() override;
    std::uintmax_t journal_bytes() const;
private:
    class StoreState;
    std::filesystem::path path_;
    std::filesystem::path journal_path_;
    std::filesystem::path portable_root_;
    mutable std::mutex mutex_;
    std::map<std::string, Track> tracks_;
    std::map<std::string, std::string> hashes_;
    std::unique_ptr<StoreState> state_;
    std::function<bool()> defer_compaction_;
    std::condition_variable cv_;
    std::thread compactor_;
    bool stopping_ = false;
    bool dirty_ = false;
    std::uint64_t sequence_ = 0;
    std::chrono::steady_clock::time_point compact_after_{};
    void append_upsert(const Track& track);
    void compact_locked();
    void compaction_loop();
};

} // namespace liveplay::library
