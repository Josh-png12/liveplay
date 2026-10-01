#pragma once
#include <filesystem>
#include <memory>
namespace liveplay::library {
// Lifetime lock for one durable store. The marker remains on disk; ownership is
// the OS handle/lock, so crashes release it without unsafe stale-file deletion.
class StoreLock {
public:
    explicit StoreLock(const std::filesystem::path& path);
    ~StoreLock();
    StoreLock(const StoreLock&) = delete;
    StoreLock& operator=(const StoreLock&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
