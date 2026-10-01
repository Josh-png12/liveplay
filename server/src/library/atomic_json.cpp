#include "liveplay/util/atomic_json.hpp"
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace liveplay::util {

namespace {
void sync_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot open file for durability sync");
    const BOOL ok = FlushFileBuffers(file); const auto error = ok ? ERROR_SUCCESS : GetLastError(); CloseHandle(file);
    if (!ok) throw std::system_error(static_cast<int>(error), std::system_category(), "cannot sync durable file");
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "cannot open file for durability sync");
    const int rc = ::fsync(fd); const int error = errno; ::close(fd);
    if (rc != 0) throw std::system_error(error, std::generic_category(), "cannot sync durable file");
#endif
}
}

void atomic_write_text(const std::filesystem::path& path, std::string_view value) {
    namespace fs = std::filesystem;
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += ".tmp";
    try {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open JSON temporary file");
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
            out.flush();
            out.close();
            if (!out) throw std::runtime_error("cannot write JSON temporary file");
        }
        sync_file(tmp);
#if defined(_WIN32)
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(), "cannot replace JSON file");
        }
#else
        fs::rename(tmp, path);
        const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) { ::fsync(directory); ::close(directory); }
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

void atomic_write_json(const std::filesystem::path& path, const nlohmann::json& value) {
    atomic_write_text(path, value.dump(2));
}

void append_durable_line(const std::filesystem::path& path, std::string_view value) {
    namespace fs = std::filesystem;
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "cannot open append journal");
    std::string line(value); line.push_back('\n'); DWORD written = 0;
    const BOOL wrote = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    const BOOL synced = wrote && written == line.size() && FlushFileBuffers(file);
    const auto error = synced ? ERROR_SUCCESS : GetLastError(); CloseHandle(file);
    if (!synced) throw std::system_error(static_cast<int>(error), std::system_category(), "cannot append durable journal");
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "cannot open append journal");
    std::string line(value); line.push_back('\n');
    const auto wrote = ::write(fd, line.data(), line.size()); const int sync_result = wrote == static_cast<ssize_t>(line.size()) ? ::fsync(fd) : -1;
    const int error = errno; ::close(fd);
    if (wrote != static_cast<ssize_t>(line.size()) || sync_result != 0) throw std::system_error(error, std::generic_category(), "cannot append durable journal");
#endif
}

nlohmann::json read_bounded_json(const std::filesystem::path& path,
                               std::uintmax_t max_bytes) {
    if (std::filesystem::file_size(path) > max_bytes)
        throw std::runtime_error("JSON store exceeds size limit");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open JSON store");
    return nlohmann::json::parse(in);
}

} // namespace liveplay::util
