#pragma once
#include "liveplay/library/library_service.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
#include <functional>
#include <set>
#include <map>
namespace liveplay::analysis {
using json = nlohmann::json;
json empty_analysis(const std::string& track_id);
struct AnalysisJob {
    // asset_path is the file handed to the provider (possibly a decoded proxy);
    // integrity_path, when set, is the original asset whose SHA-256 is content_hash.
    std::string id, track_id, content_hash, provider, asset_path, integrity_path;
    int analysis_version = 1;
    std::string status = "pending", error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AnalysisJob, id, track_id, content_hash,
    provider, asset_path, integrity_path, analysis_version, status, error)
json worker_request(const AnalysisJob& job);
json worker_response(const AnalysisJob& job, const json& response);
class TrackAnalysisRepository {
public:
    virtual ~TrackAnalysisRepository() = default;
    virtual std::optional<json> find(const std::string& track_id) const = 0;
    virtual void save(const json& analysis) = 0;
};
class JsonTrackAnalysisRepository final : public TrackAnalysisRepository {
public:
    explicit JsonTrackAnalysisRepository(std::filesystem::path root) : root_(std::move(root)) {}
    std::optional<json> find(const std::string& track_id) const override;
    void save(const json& analysis) override;
private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
};
// A future external process adapter exchanges bounded JSON envelopes. execute()
// is called only by the scheduler worker, never HTTP/render/audio callbacks.
class AnalysisProvider {
public:
    virtual ~AnalysisProvider() = default;
    virtual std::string id() const = 0;
    virtual json capabilities() const = 0;
    virtual json execute(const AnalysisJob& job) = 0;
    // Abort the request being executed (called from another thread) and let a
    // long-lived worker process go once the queue is idle. Defaults: nothing to do.
    virtual void cancel_current() noexcept {}
    virtual void release() noexcept {}
};
// Providers whose result is not the legacy per-track record (e.g. musical analysis)
// receive it through a sink so it never overwrites another provider's data.
struct ProviderSink {
    std::function<void(const AnalysisJob&, json& result)> accept;
    std::function<void(const AnalysisJob&, const std::string& error)> fail;
};
class AnalysisScheduler {
public:
    AnalysisScheduler(TrackAnalysisRepository& repository, std::filesystem::path root,
        std::vector<std::shared_ptr<AnalysisProvider>> providers = {},
        std::function<bool()> low_impact = {});
    ~AnalysisScheduler();
    // `force` re-runs a finished (completed/failed/cancelled) job; otherwise the existing job is returned.
    AnalysisJob enqueue(const library::Track& track, const std::string& provider, bool force = false, const std::string& input_path = "");
    void set_sink(const std::string& provider, ProviderSink sink);
    // Cancel pending jobs (and the running one when it matches). Empty `track_ids` = every job;
    // empty `provider` = every provider. Returns the number of jobs cancelled.
    std::size_t cancel(const std::vector<std::string>& track_ids = {}, const std::string& provider = "");
    std::optional<AnalysisJob> job(const std::string& track_id, const std::string& provider) const;
    bool has_provider(const std::string& id) const;
    json provider_capabilities(const std::string& id) const;
    json jobs() const;
    json capabilities() const;
    bool available() const { return !providers_.empty(); }
    void stop();
private:
    TrackAnalysisRepository& repository_;
    std::filesystem::path root_;
    std::vector<std::shared_ptr<AnalysisProvider>> providers_;
    std::function<bool()> low_impact_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, AnalysisJob> jobs_;
    std::deque<std::string> queue_;
    std::map<std::string, ProviderSink> sinks_;
    std::set<std::string> cancelled_;
    std::string running_id_;
    bool stopping_ = false;
    std::thread worker_;
    void persist() const;
    void run();
};
}
