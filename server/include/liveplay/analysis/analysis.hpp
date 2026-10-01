#pragma once
#include "liveplay/library/library_service.hpp"
#include <condition_variable>
#include <deque>
#include <thread>
#include <functional>
namespace liveplay::analysis {
using json = nlohmann::json;
json empty_analysis(const std::string& track_id);
struct AnalysisJob {
    std::string id, track_id, content_hash, provider, asset_path;
    int analysis_version = 1;
    std::string status = "pending", error;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AnalysisJob, id, track_id, content_hash,
    provider, asset_path, analysis_version, status, error)
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
};
class AnalysisScheduler {
public:
    AnalysisScheduler(TrackAnalysisRepository& repository, std::filesystem::path root,
        std::vector<std::shared_ptr<AnalysisProvider>> providers = {},
        std::function<bool()> low_impact = {});
    ~AnalysisScheduler();
    AnalysisJob enqueue(const library::Track& track, const std::string& provider);
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
    bool stopping_ = false;
    std::thread worker_;
    void persist() const;
    void run();
};
}
