#pragma once
#include "liveplay/analysis/analysis.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
namespace liveplay::analysis {
// One isolated process per request keeps protocol failure and crash recovery
// simple: a bad worker is killed, and the next job starts a clean process.
class ExternalAnalysisProvider final : public AnalysisProvider {
public:
    ExternalAnalysisProvider(std::filesystem::path executable,
                             std::chrono::milliseconds timeout = std::chrono::seconds(30));
    ~ExternalAnalysisProvider() override;
    std::string id() const override { return "external"; }
    json capabilities() const override;
    json execute(const AnalysisJob& job) override;
    bool health_check();
    void cancel() noexcept;
private:
    std::filesystem::path executable_;
    std::chrono::milliseconds timeout_;
    std::atomic<bool> cancel_requested_{false};
    std::mutex process_mutex_;
#if defined(_WIN32)
    void* active_process_ = nullptr;
#else
    int active_pid_ = -1;
#endif
    json exchange(const json& request);
};
}
