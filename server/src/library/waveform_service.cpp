#include "liveplay/library/waveform_service.hpp"
#include "liveplay/meta/waveform.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <algorithm>

namespace liveplay::library {
namespace fs = std::filesystem;

WaveformService::WaveformService(fs::path cache_root, std::function<bool()> low_impact)
    : root_(std::move(cache_root)), low_impact_(std::move(low_impact)), worker_([this] { run(); }) {
    std::error_code ec;
    fs::create_directories(root_, ec);
}
WaveformService::~WaveformService() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all(); if (worker_.joinable()) worker_.join();
}
fs::path WaveformService::cache_file(const Track& track) const { return root_ / (track.id + ".json"); }
void WaveformService::request(const Track& track) {
    std::lock_guard lock(mutex_);
    const auto cached = cache_file(track);
    if (fs::exists(cached)) {
        try {
            const auto doc = util::read_bounded_json(cached, 8ull * 1024 * 1024);
            if (doc.value("content_hash", "") == track.content_hash) { states_[track.id] = "ready"; return; }
        } catch (...) {}
    }
    const auto it = states_.find(track.id);
    if (it != states_.end() && (it->second == "queued" || it->second == "running")) return;
    states_[track.id] = "queued"; errors_.erase(track.id); queue_.push_back({track}); cv_.notify_one();
}
nlohmann::json WaveformService::status(const Track& track) const {
    std::lock_guard lock(mutex_);
    const auto file = cache_file(track);
    if (fs::exists(file)) {
        try {
            auto doc = util::read_bounded_json(file, 8ull * 1024 * 1024);
            if (doc.value("content_hash", "") == track.content_hash) { doc["status"] = "ready"; return doc; }
        } catch (...) {}
    }
    const auto state = states_.contains(track.id) ? states_.at(track.id) : "absent";
    return {{"track_id", track.id}, {"status", state}, {"error", errors_.contains(track.id) ? errors_.at(track.id) : ""}};
}
void WaveformService::run() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_); cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return; job = std::move(queue_.front()); queue_.pop_front(); states_[job.track.id] = "running";
        }
        try {
            while (low_impact_ && low_impact_()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::lock_guard lock(mutex_); if (stopping_) return;
            }
            const auto wf = meta::compute_waveform(util::utf8_to_path(job.track.asset_path));
            if (!wf.ok) throw std::runtime_error("waveform decode failed");
            nlohmann::json channels = nlohmann::json::array();
            for (const auto& channel : wf.channels) channels.push_back({{"peak", channel.peak}, {"rms", channel.rms}});
            const nlohmann::json doc{{"schema_version", 1}, {"track_id", job.track.id}, {"content_hash", job.track.content_hash},
                {"bucket_count", wf.bucket_count}, {"duration_ms", wf.duration.count()}, {"sample_rate", wf.sample_rate},
                {"source_channels", wf.source_channels}, {"channels", std::move(channels)}};
            util::atomic_write_json(cache_file(job.track), doc);
            std::lock_guard lock(mutex_); states_[job.track.id] = "ready";
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_); states_[job.track.id] = "failed"; errors_[job.track.id] = e.what();
        } catch (...) {
            std::lock_guard lock(mutex_); states_[job.track.id] = "failed"; errors_[job.track.id] = "unknown waveform failure";
        }
    }
}
} // namespace liveplay::library
