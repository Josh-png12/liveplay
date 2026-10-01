#include "liveplay/analysis/analysis.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/library/maintenance.hpp"
#include <algorithm>
#include <cctype>
namespace liveplay::analysis {
namespace fs = std::filesystem;
namespace {
bool valid_track(const std::string& id) {
    return id.size() == 70 && id.starts_with("track_") && std::all_of(id.begin()+6, id.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
void validate(const json& record) {
    if (!record.is_object() || !valid_track(record.at("track_id").get<std::string>()) || record.at("analysis_version") != 1)
        throw std::invalid_argument("invalid analysis schema/identity");
    const auto state = record.value("status", "pending");
    if (state != "pending" && state != "running" && state != "completed" && state != "failed") throw std::invalid_argument("invalid analysis status");
    for (const auto* section : {"providers", "basic", "loudness", "rhythm", "tonality", "structure"})
        if (record.contains(section) && !record[section].is_null() && !record[section].is_object()) throw std::invalid_argument("invalid analysis section");
    if (record.dump().size() > 8ull * 1024 * 1024) throw std::length_error("analysis too large");
}
}
json empty_analysis(const std::string& track_id) {
    return {{"track_id", track_id}, {"analysis_version", 1}, {"status", "pending"}, {"providers", json::object()},
        {"basic", {{"duration_ms", nullptr}, {"peak_dbfs", nullptr}, {"rms_dbfs", nullptr}}},
        {"loudness", {{"integrated_lufs", nullptr}, {"true_peak_dbtp", nullptr}, {"lra", nullptr}}},
        {"rhythm", {{"bpm", nullptr}, {"confidence", nullptr}, {"beats", json::array()}}},
        {"tonality", {{"key", nullptr}, {"scale", nullptr}, {"camelot", nullptr}, {"confidence", nullptr}}},
        {"structure", {{"sections", json::array()}}}};
}
json worker_request(const AnalysisJob& job) {
    return {{"protocol_version", 1}, {"operation", "analyze"}, {"job", job}};
}
json worker_response(const AnalysisJob& job, const json& response) {
    if (response.dump().size() > 8ull * 1024 * 1024) throw std::length_error("worker response too large");
    if (response.at("protocol_version") != 1 || response.at("job_id") != job.id || response.at("content_hash") != job.content_hash)
        throw std::invalid_argument("worker correlation/version mismatch");
    auto result = response.at("analysis"); validate(result);
    if (result.at("track_id") != job.track_id || result.value("status", "pending") != "completed")
        throw std::invalid_argument("worker returned failed/stale analysis");
    return result;
}
std::optional<json> JsonTrackAnalysisRepository::find(const std::string& id) const {
    if (!valid_track(id)) throw std::invalid_argument("invalid track ID");
    std::lock_guard lock(mutex_);
    const auto path = root_ / (id + ".json");
    if (!fs::exists(path)) return std::nullopt;
    auto record = util::read_bounded_json(path, 8ull * 1024 * 1024); validate(record);
    if (record.at("track_id") != id) throw std::runtime_error("analysis identity mismatch");
    return record;
}
void JsonTrackAnalysisRepository::save(const json& record) {
    validate(record); std::lock_guard lock(mutex_);
    util::atomic_write_json(root_ / (record.at("track_id").get<std::string>() + ".json"), record);
}
AnalysisScheduler::AnalysisScheduler(TrackAnalysisRepository& repository, fs::path root,
    std::vector<std::shared_ptr<AnalysisProvider>> providers, std::function<bool()> low_impact)
    : repository_(repository), root_(std::move(root)), providers_(std::move(providers)), low_impact_(std::move(low_impact)) {
    const auto path = root_ / "jobs.json";
    if (fs::exists(path)) {
        const auto doc = util::read_bounded_json(path, 8ull * 1024 * 1024);
        if (doc.at("schema_version") != 1 || !doc.at("jobs").is_array() || doc.at("jobs").size() > 256)
            throw std::runtime_error("invalid analysis jobs store");
        for (const auto& item : doc.at("jobs")) {
            auto job = item.get<AnalysisJob>();
            const auto path = fs::u8path(job.asset_path);
            if (path.is_relative()) {
                const auto relative = path.lexically_normal();
                if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("invalid portable analysis asset path");
                const auto resolved = fs::absolute(root_.parent_path() / relative).u8string();
                job.asset_path = std::string(reinterpret_cast<const char*>(resolved.data()), resolved.size());
            }
            if (!valid_track(job.track_id) || job.analysis_version != 1 || job.id != "job_" + job.track_id + "_" + job.provider)
                throw std::runtime_error("invalid analysis job");
            if (job.status == "running") job.status = "pending";
            if (!jobs_.emplace(job.id, job).second) throw std::runtime_error("duplicate analysis job");
            if (job.status == "pending") queue_.push_back(job.id);
        }
    }
    worker_ = std::thread([this] { run(); });
}
AnalysisScheduler::~AnalysisScheduler() { stop(); }
void AnalysisScheduler::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all(); if (worker_.joinable()) worker_.join();
}
void AnalysisScheduler::persist() const {
    json values = json::array(); for (const auto& [id, job] : jobs_) {
        auto stored = job;
        const auto relative = fs::u8path(job.asset_path).lexically_relative(root_.parent_path());
        if (!relative.empty() && *relative.begin() != "..") {
            const auto portable = relative.generic_u8string();
            stored.asset_path = std::string(reinterpret_cast<const char*>(portable.data()), portable.size());
        }
        values.push_back(stored);
    }
    util::atomic_write_json(root_ / "jobs.json", {{"schema_version", 1}, {"jobs", values}});
}
AnalysisJob AnalysisScheduler::enqueue(const library::Track& track, const std::string& provider) {
    if (provider.empty() || provider.size() > 64 || !std::all_of(provider.begin(), provider.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
        throw std::invalid_argument("invalid provider ID");
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("analysis scheduler stopped");
    AnalysisJob job;
    job.id = "job_" + track.id + "_" + provider; job.track_id = track.id; job.content_hash = track.content_hash;
    job.provider = provider; job.asset_path = track.asset_path;
    if (const auto it = jobs_.find(job.id); it != jobs_.end() && it->second.status != "failed") return it->second;
    if (jobs_.size() >= 256 && !jobs_.contains(job.id)) throw std::length_error("analysis job limit reached");
    const auto previous = jobs_;
    jobs_[job.id] = job;
    try { persist(); } catch (...) { jobs_ = previous; throw; }
    queue_.push_back(job.id); cv_.notify_one(); return job;
}
json AnalysisScheduler::jobs() const { std::lock_guard lock(mutex_); json values = json::array(); for (const auto& [id,j] : jobs_) values.push_back(j); return values; }
json AnalysisScheduler::capabilities() const {
    json result = {{"basic", false}, {"loudness", false}, {"rhythm", false}, {"tonality", false}, {"structure", false}, {"semantic", false}, {"stems", false}};
    for (const auto& provider : providers_) for (auto& [key,value] : result.items()) value = value.get<bool>() || provider->capabilities().value(key, false);
    return result;
}
void AnalysisScheduler::run() {
    while (true) {
        AnalysisJob job;
        {
            std::unique_lock lock(mutex_); cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = jobs_.at(queue_.front()); queue_.pop_front(); job.status = "running";
            jobs_[job.id] = job;
            try { persist(); } catch (const std::exception& e) { jobs_[job.id].status = "failed"; jobs_[job.id].error = e.what(); stopping_ = true; return; }
        }
        auto result = empty_analysis(job.track_id);
        const auto fail = [&](const std::string& error) {
            job.status = "failed"; job.error = error; result = empty_analysis(job.track_id);
            result["status"] = "failed"; result["providers"][job.provider] = {{"status", "failed"}, {"error", job.error}};
            try { repository_.save(result); } catch (...) {}
        };
        try {
            while (low_impact_ && low_impact_()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::lock_guard pause_lock(mutex_); if (stopping_) return;
            }
            const auto provider = std::find_if(providers_.begin(), providers_.end(), [&](const auto& p) { return p->id() == job.provider; });
            if (provider == providers_.end()) throw std::runtime_error("analysis provider unavailable");
            // Independent file/hash validation prevents stale worker output.
            if (library::hash_asset(job.asset_path) != job.content_hash) throw std::runtime_error("analysis asset integrity mismatch");
            result = (*provider)->execute(job); validate(result);
            if (library::hash_asset(job.asset_path) != job.content_hash) throw std::runtime_error("asset changed during analysis");
            if (result.at("track_id") != job.track_id) throw std::runtime_error("provider returned another track");
            result["status"] = "completed"; result["content_hash"] = job.content_hash;
            result["providers"][job.provider] = {{"status", "completed"}};
            repository_.save(result); job.status = "completed";
        } catch (const std::exception& e) {
            fail(e.what());
        } catch (...) { fail("unknown analysis provider failure"); }
        std::lock_guard lock(mutex_); jobs_[job.id] = job;
        try { persist(); } catch (...) { stopping_ = true; return; }
    }
}
}
