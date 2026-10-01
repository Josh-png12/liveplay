#include "liveplay/util/atomic_json.hpp"
#include <fstream>
#include <stdexcept>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace liveplay::util {

void atomic_write_json(const std::filesystem::path& path, const nlohmann::json& value) {
    namespace fs = std::filesystem;
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += ".tmp";
    try {
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open JSON temporary file");
            out << value.dump(2);
            out.flush();
            out.close();
            if (!out) throw std::runtime_error("cannot write JSON temporary file");
        }
#if defined(_WIN32)
        if (!MoveFileExW(tmp.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(), "cannot replace JSON file");
        }
#else
        fs::rename(tmp, path);
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
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
