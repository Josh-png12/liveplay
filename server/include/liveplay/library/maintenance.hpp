#pragma once
#include "liveplay/library/library_service.hpp"
#include <thread>
#include <atomic>
namespace liveplay::library {
std::string hash_asset(const std::filesystem::path& path);
// One bounded maintenance operation; no playback references or automatic deletion.
class LibraryMaintenance {
public:
    LibraryMaintenance(LibraryService& library, std::filesystem::path assets);
    ~LibraryMaintenance();
    nlohmann::json start(const std::string& operation, const std::vector<std::string>& selected = {});
    nlohmann::json status() const;
private:
    LibraryService& library_;
    std::filesystem::path assets_;
    mutable std::mutex mutex_;
    nlohmann::json status_ = {{"status", "idle"}, {"orphans", nlohmann::json::array()}};
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    void run(std::string operation, std::vector<std::string> selected);
};
}
