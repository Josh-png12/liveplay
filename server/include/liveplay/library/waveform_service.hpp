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

// Library waveform cache. It runs independently of AudioEngine and stores a
// bounded, display-ready envelope keyed by immutable Track ID/content hash.
class WaveformService {
public:
    WaveformService(std::filesystem::path cache_root, std::function<bool()> low_impact = {});
    ~WaveformService();
    WaveformService(const WaveformService&) = delete;
    void request(const Track& track);
    nlohmann::json status(const Track& track) const;
private:
    struct Job { Track track; };
    std::filesystem::path root_;
    std::function<bool()> low_impact_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::unordered_map<std::string, std::string> states_;
    std::unordered_map<std::string, std::string> errors_;
    bool stopping_ = false;
    std::thread worker_;
    std::filesystem::path cache_file(const Track& track) const;
    void run();
};

} // namespace liveplay::library
