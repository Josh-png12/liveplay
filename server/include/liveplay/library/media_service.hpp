#pragma once

#include "liveplay/library/track.hpp"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace liveplay::library {

// Offline media operations. This service owns no playback state and never
// executes from AudioEngine's render callback.
TrackMedia probe_media_file(const std::filesystem::path& source);

class AudioProxyService {
public:
    AudioProxyService(std::filesystem::path cache_root, std::function<bool()> low_impact = {},
                      std::function<void(Track)> on_ready = {});
    ~AudioProxyService();
    AudioProxyService(const AudioProxyService&) = delete;
    void request(const Track& track);
    nlohmann::json status(const Track& track) const;
    std::filesystem::path proxy_path(const Track& track) const;
    bool busy() const;
    nlohmann::json cache_status() const;
    bool clear_regenerable_cache(std::string& error);
private:
    std::filesystem::path root_;
    std::function<bool()> low_impact_;
    std::function<void(Track)> on_ready_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Track> queue_;
    std::unordered_map<std::string, std::string> states_, errors_;
    bool stopping_ = false;
    std::thread worker_;
    void run();
};
} // namespace liveplay::library
