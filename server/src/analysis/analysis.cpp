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
constexpr std::size_t max_jobs = 4096;
std::string to_utf8(const fs::path& path) { const auto text = path.u8string(); return std::string(reinterpret_cast<const char*>(text.data()), text.size()); }
// Relative paths in the store are relative to the plus directory so the whole directory stays portable.
std::string resolve_stored(const fs::path& plus_root, const std::string& stored) {
    if (stored.empty()) return stored;
    const auto path = fs::u8path(stored);
    if (!path.is_relative()) return stored;
    const auto relative = path.lexically_normal();
    if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("invalid portable analysis asset path");
    return to_utf8(fs::absolute(plus_root / relative));
}
std::string portable_stored(const fs::path& plus_root, const std::string& absolute) {
    if (absolute.empty()) return absolute;
    const auto relative = fs::u8path(absolute).lexically_relative(plus_root);
    if (!relative.empty() && *relative.begin() != "..") return to_utf8(relative.generic_u8string());
    return absolute;
}
bool finished(const AnalysisJob& job) { return job.status == "completed" || job.status == "failed" || job.status == "cancelled"; }
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
        const auto doc = util::read_bounded_json(path, 16ull * 1024 * 1024);
        if (doc.at("schema_version") != 1 || !doc.at("jobs").is_array() || doc.at("jobs").size() > max_jobs)
            throw std::runtime_error("invalid analysis jobs store");
        for (const auto& item : doc.at("jobs")) {
            auto job = item.get<AnalysisJob>();
            job.asset_path = resolve_stored(root_.parent_path(), job.asset_path);
            job.integrity_path = resolve_stored(root_.parent_path(), job.integrity_path);
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
    cv_.notify_all();
    for (const auto& provider : providers_) provider->cancel_current();
    if (worker_.joinable()) worker_.join();
    for (const auto& provider : providers_) provider->release();
}
void AnalysisScheduler::persist() const {
    json values = json::array(); for (const auto& [id, job] : jobs_) {
        auto stored = job;
        stored.asset_path = portable_stored(root_.parent_path(), job.asset_path);
        stored.integrity_path = portable_stored(root_.parent_path(), job.integrity_path);
        values.push_back(stored);
    }
    util::atomic_write_json(root_ / "jobs.json", {{"schema_version", 1}, {"jobs", values}});
}
void AnalysisScheduler::set_sink(const std::string& provider, ProviderSink sink) {
    std::lock_guard lock(mutex_); sinks_[provider] = std::move(sink);
}
bool AnalysisScheduler::has_provider(const std::string& id) const {
    return std::any_of(providers_.begin(), providers_.end(), [&](const auto& p) { return p->id() == id; });
}
json AnalysisScheduler::provider_capabilities(const std::string& id) const {
    for (const auto& provider : providers_) if (provider->id() == id) return provider->capabilities();
    return json::object();
}
AnalysisJob AnalysisScheduler::enqueue(const library::Track& track, const std::string& provider, bool force, const std::string& input_path) {
    if (provider.empty() || provider.size() > 64 || !std::all_of(provider.begin(), provider.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }))
        throw std::invalid_argument("invalid provider ID");
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("analysis scheduler stopped");
    AnalysisJob job;
    job.id = "job_" + track.id + "_" + provider; job.track_id = track.id; job.content_hash = track.content_hash;
    job.provider = provider; job.asset_path = input_path.empty() ? track.asset_path : input_path;
    if (!input_path.empty() && input_path != track.asset_path) job.integrity_path = track.asset_path;
    if (const auto it = jobs_.find(job.id); it != jobs_.end()) {
        if (it->second.status == "pending" || it->second.status == "running") return it->second;
        if (it->second.status == "completed" && !force) return it->second;
    }
    if (jobs_.size() >= max_jobs && !jobs_.contains(job.id)) {
        for (auto it = jobs_.begin(); it != jobs_.end() && jobs_.size() >= max_jobs;) it = finished(it->second) ? jobs_.erase(it) : std::next(it);
        if (jobs_.size() >= max_jobs) throw std::length_error("analysis job limit reached");
    }
    const auto previous = jobs_;
    jobs_[job.id] = job;
    try { persist(); } catch (...) { jobs_ = previous; throw; }
    cancelled_.erase(job.id);
    queue_.push_back(job.id); cv_.notify_one(); return job;
}
std::size_t AnalysisScheduler::cancel(const std::vector<std::string>& track_ids, const std::string& provider) {
    std::size_t count = 0; bool kill_running = false;
    {
        std::lock_guard lock(mutex_);
        const auto selected = [&](const AnalysisJob& job) {
            return (provider.empty() || job.provider == provider) &&
                (track_ids.empty() || std::find(track_ids.begin(), track_ids.end(), job.track_id) != track_ids.end());
        };
        for (auto it = queue_.begin(); it != queue_.end();) {
            auto& job = jobs_.at(*it);
            if (job.status == "pending" && selected(job)) { job.status = "cancelled"; job.error = "cancelled"; it = queue_.erase(it); ++count; }
            else ++it;
        }
        if (!running_id_.empty()) if (auto it = jobs_.find(running_id_); it != jobs_.end() && it->second.status == "running" && selected(it->second)) {
            cancelled_.insert(running_id_); kill_running = true; ++count;
        }
        if (count) { try { persist(); } catch (...) {} }
    }
    if (kill_running) for (const auto& p : providers_) p->cancel_current();
    return count;
}
std::optional<AnalysisJob> AnalysisScheduler::job(const std::string& track_id, const std::string& provider) const {
    std::lock_guard lock(mutex_);
    if (const auto it = jobs_.find("job_" + track_id + "_" + provider); it != jobs_.end()) return it->second;
    return std::nullopt;
}
json AnalysisScheduler::jobs() const { std::lock_guard lock(mutex_); json values = json::array(); for (const auto& [id,j] : jobs_) values.push_back(j); return values; }
json AnalysisScheduler::capabilities() const {
    json result = {{"basic", false}, {"loudness", false}, {"rhythm", false}, {"tonality", false}, {"structure", false}, {"semantic", false}, {"stems", false}};
    for (const auto& provider : providers_) for (auto& [key,value] : result.items()) value = value.get<bool>() || provider->capabilities().value(key, false);
    return result;
}
void AnalysisScheduler::run() {
    while (true) {
        AnalysisJob job; std::shared_ptr<AnalysisProvider> active; ProviderSink sink;
        {
            std::unique_lock lock(mutex_); cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            const auto it = jobs_.find(queue_.front()); queue_.pop_front();
            if (it == jobs_.end() || it->second.status != "pending") continue;
            job = it->second; job.status = "running";
            jobs_[job.id] = job; running_id_ = job.id; cancelled_.erase(job.id);
            if (const auto s = sinks_.find(job.provider); s != sinks_.end()) sink = s->second;
            try { persist(); } catch (const std::exception& e) { jobs_[job.id].status = "failed"; jobs_[job.id].error = e.what(); running_id_.clear(); stopping_ = true; return; }
        }
        auto result = empty_analysis(job.track_id);
        const auto was_cancelled = [&] { std::lock_guard lock(mutex_); return cancelled_.contains(job.id); };
        const auto fail = [&](const std::string& error) {
            job.status = "failed"; job.error = error;
            if (sink.fail) { try { sink.fail(job, error); } catch (...) {} return; }
            result = empty_analysis(job.track_id);
            result["status"] = "failed"; result["providers"][job.provider] = {{"status", "failed"}, {"error", job.error}};
            try { repository_.save(result); } catch (...) {}
        };
        try {
            while (low_impact_ && low_impact_()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::lock_guard pause_lock(mutex_); if (stopping_) return;
                if (cancelled_.contains(job.id)) break;
            }
            if (was_cancelled()) throw std::runtime_error("cancelled");
            const auto provider = std::find_if(providers_.begin(), providers_.end(), [&](const auto& p) { return p->id() == job.provider; });
            if (provider == providers_.end()) throw std::runtime_error("analysis provider unavailable");
            active = *provider;
            // Independent file/hash validation prevents stale worker output.
            const auto integrity = job.integrity_path.empty() ? job.asset_path : job.integrity_path;
            if (library::hash_asset(integrity) != job.content_hash) throw std::runtime_error("analysis asset integrity mismatch");
            if (was_cancelled()) throw std::runtime_error("cancelled");
            result = (*provider)->execute(job);
            if (was_cancelled()) throw std::runtime_error("cancelled");
            if (!sink.accept) validate(result);
            if (library::hash_asset(integrity) != job.content_hash) throw std::runtime_error("asset changed during analysis");
            if (result.at("track_id") != job.track_id) throw std::runtime_error("provider returned another track");
            result["status"] = "completed"; result["content_hash"] = job.content_hash;
            result["providers"][job.provider] = {{"status", "completed"}};
            if (sink.accept) sink.accept(job, result); else repository_.save(result);
            job.status = "completed"; job.error.clear();
        } catch (const std::exception& e) {
            if (was_cancelled()) { job.status = "cancelled"; job.error = "cancelled"; } else fail(e.what());
        } catch (...) { fail("unknown analysis provider failure"); }
        bool idle = false;
        {
            std::lock_guard lock(mutex_); jobs_[job.id] = job; running_id_.clear(); cancelled_.erase(job.id); idle = queue_.empty();
            try { persist(); } catch (...) { stopping_ = true; return; }
        }
        if (idle && active) active->release();
    }
}
}
