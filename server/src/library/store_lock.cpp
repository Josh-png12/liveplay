#include "liveplay/library/store_lock.hpp"
#include <stdexcept>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace liveplay::library {
struct StoreLock::Impl {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};
StoreLock::StoreLock(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
    std::filesystem::create_directories(path.parent_path());
#if defined(_WIN32)
    impl_->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) throw std::runtime_error("LivePlay+ library is already open by another process");
#else
    impl_->fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (impl_->fd < 0 || ::flock(impl_->fd, LOCK_EX | LOCK_NB) != 0) {
        if (impl_->fd >= 0) ::close(impl_->fd);
        throw std::runtime_error("LivePlay+ library is already open by another process");
    }
#endif
}
StoreLock::~StoreLock() {
#if defined(_WIN32)
    if (impl_->handle != INVALID_HANDLE_VALUE) CloseHandle(impl_->handle);
#else
    if (impl_->fd >= 0) { ::flock(impl_->fd, LOCK_UN); ::close(impl_->fd); }
#endif
}
}
