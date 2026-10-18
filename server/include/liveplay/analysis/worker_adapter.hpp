#pragma once
#include "liveplay/analysis/analysis.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>
namespace liveplay::analysis {
// One isolated process per request keeps protocol failure and crash recovery
// simple: a bad worker is killed, and the next job starts a clean process.
struct ExternalWorkerOptions {
    std::vector<std::string> arguments;  // UTF-8 command-line arguments after the executable
    bool low_priority = false;           // run the worker below normal priority (Windows)
    // Keep one worker process alive between requests (Windows). Used when start-up
    // dominates (a scientific Python stack); crash/timeout/cancel always discard it.
    bool persistent = false;
};
class ExternalAnalysisProvider : public AnalysisProvider {
public:
    ExternalAnalysisProvider(std::filesystem::path executable,
                             std::chrono::milliseconds timeout = std::chrono::seconds(30),
                             std::string provider_id = "external",
                             json capabilities = {}, ExternalWorkerOptions options = {});
    ~ExternalAnalysisProvider() override;
    std::string id() const override { return provider_id_; }
    json capabilities() const override;
    json execute(const AnalysisJob& job) override;
    bool health_check();
    // Full health reply (`status:"ok"` plus whatever the worker reports, e.g. capabilities).
    json health_info();
    // Sends a payload-less request of `type` (e.g. "warmup") and returns the correlated reply.
    json call(const std::string& type);
    void cancel() noexcept;
    void cancel_current() noexcept override { cancel(); }
    void release() noexcept override;
private:
    ExternalWorkerOptions options_;
    std::mutex exchange_mutex_;
    std::string pending_output_;
#if defined(_WIN32)
    void* persistent_in_ = nullptr;
    void* persistent_out_ = nullptr;
    void* persistent_process_ = nullptr;
    json exchange_persistent(const json& request);
    void discard_persistent() noexcept;
    std::wstring command_line() const;
    unsigned long creation_flags() const;
#endif
    std::filesystem::path executable_;
    std::chrono::milliseconds timeout_;
    std::string provider_id_;
    json capabilities_;
    std::atomic<bool> cancel_requested_{false};
    std::mutex process_mutex_;
#if defined(_WIN32)
    void* active_process_ = nullptr;
#else
    int active_pid_ = -1;
#endif
    json exchange(const json& request, bool allow_persistent = true);
};
}
