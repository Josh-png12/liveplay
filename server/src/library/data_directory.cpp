#include "liveplay/library/data_directory.hpp"
#include "liveplay/library/track_repository.hpp"
#include "liveplay/util/unicode_path.hpp"
#include "liveplay/util/atomic_json.hpp"
#include <cstdlib>
#include <chrono>
#if defined(_WIN32)
#include <windows.h>
#endif
namespace liveplay::library {
namespace fs = std::filesystem;
std::filesystem::path resolve_plus_directory(const std::string& override_path, Environment env) {
    if (!env) env = [](const char* name) {
#if defined(_WIN32)
        const auto key = util::utf8_to_path(name).wstring();
        const auto count = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
        if (!count) return std::string{};
        std::wstring value(count, L'\0');
        value.resize(GetEnvironmentVariableW(key.c_str(), value.data(), count));
        return util::path_to_utf8(fs::path(value));
#else
        const char* value = std::getenv(name); return value ? std::string(value) : std::string{};
#endif
    };
    auto override_value = env("LIVEPLAY_PLUS_DATA_DIR");
    if (override_value.empty()) override_value = override_path;
    if (!override_value.empty()) return fs::absolute(util::utf8_to_path(override_value)).lexically_normal();
#if defined(_WIN32)
    const auto base = env("LOCALAPPDATA");
    if (!base.empty()) return util::utf8_to_path(base) / "LivePlay" / "plus";
#elif defined(__APPLE__)
    const auto base = env("HOME");
    if (!base.empty()) return util::utf8_to_path(base) / "Library" / "Application Support" / "LivePlay" / "plus";
#else
    const auto xdg = env("XDG_DATA_HOME");
    if (!xdg.empty()) return util::utf8_to_path(xdg) / "LivePlay" / "plus";
    const auto base = env("HOME");
    if (!base.empty()) return util::utf8_to_path(base) / ".local" / "share" / "LivePlay" / "plus";
#endif
    throw std::runtime_error("Plus user data directory unavailable; set LIVEPLAY_PLUS_DATA_DIR");
}
bool migrate_plus_directory(const fs::path& legacy, const fs::path& destination) {
    if (!fs::exists(legacy / "catalog.json") || fs::exists(destination / "library" / "catalog.json")) return false;
    const auto temporary = fs::path(destination.native() + fs::path(".migration").native());
    if (fs::exists(destination) || fs::exists(temporary))
        throw std::runtime_error("Plus migration destination exists without catalog; inspect it before recovery");
    // Validate before copying; a corrupt legacy catalog is retained for explicit recovery.
    if (fs::is_symlink(legacy)) throw std::runtime_error("legacy directory is a symlink");
    for (const auto& entry : fs::recursive_directory_iterator(legacy))
        if (entry.is_symlink()) throw std::runtime_error("legacy store contains a symlink");
    try { JsonTrackRepository checked(legacy / "catalog.json"); }
    catch (...) { try { backup_corrupt_catalog(legacy / "catalog.json"); } catch (...) {} throw; }
    fs::create_directories(temporary / "library");
    for (const auto& entry : fs::directory_iterator(legacy)) {
        if (entry.is_symlink()) throw std::runtime_error("legacy store contains a symlink");
        const auto target = entry.path().filename() == "assets" ? temporary / "assets"
            : temporary / "library" / entry.path().filename();
        fs::copy(entry.path(), target, fs::copy_options::recursive);
    }
    auto catalog = util::read_bounded_json(temporary / "library" / "catalog.json", 64ull * 1024 * 1024);
    for (auto& track : catalog.at("tracks")) {
        const auto original = util::utf8_to_path(track.at("asset_path").get<std::string>());
        if (original.parent_path().lexically_normal() != (legacy / "assets").lexically_normal())
            throw std::runtime_error("legacy asset outside owned assets directory");
        const auto portable = (fs::path("assets") / original.filename()).generic_u8string();
        track["asset_path"] = std::string(reinterpret_cast<const char*>(portable.data()), portable.size());
    }
    util::atomic_write_json(temporary / "library" / "catalog.json", catalog);
    if (fs::exists(temporary / "library" / "imports")) {
        for (const auto& entry : fs::directory_iterator(temporary / "library" / "imports")) {
            if (entry.path().extension() != ".json") continue;
            auto journal = util::read_bounded_json(entry.path(), 8ull * 1024 * 1024);
            if (journal.value("origin", "") == "upload") {
                for (auto& file : journal.at("files")) {
                    const auto old_path = util::utf8_to_path(file.at("source").at("path").get<std::string>());
                    const auto relative = old_path.lexically_relative(legacy / "uploads");
                    if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("invalid legacy upload path");
                    file["source"]["path"] = util::path_to_utf8(destination / "library" / "uploads" / relative);
                }
                util::atomic_write_json(entry.path(), journal);
            }
        }
    }
    fs::create_directories(temporary / "cache"); fs::create_directories(temporary / "analysis");
    fs::create_directories(destination.parent_path());
    fs::rename(temporary, destination);
    return true;
}
void backup_corrupt_catalog(const fs::path& catalog) {
    if (!fs::is_regular_file(catalog)) return;
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    fs::copy_file(catalog, catalog.parent_path() / ("catalog.corrupt." + std::to_string(stamp) + ".json"));
}
}
