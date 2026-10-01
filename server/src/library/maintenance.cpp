#include "liveplay/library/maintenance.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <sodium.h>
#include <fstream>
#include <array>
#include <set>
#include <algorithm>
namespace liveplay::library {
namespace fs = std::filesystem;
std::string hash_asset(const fs::path& path) {
    if (sodium_init() < 0) throw std::runtime_error("crypto unavailable");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read asset");
    crypto_hash_sha256_state state; crypto_hash_sha256_init(&state);
    std::array<char, 65536> buffer{};
    while (in) {
        in.read(buffer.data(), buffer.size());
        crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(buffer.data()), in.gcount());
    }
    if (!in.eof()) throw std::runtime_error("asset read failed");
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    return "sha256:" + std::string(hex.data());
}
LibraryMaintenance::LibraryMaintenance(LibraryService& library, fs::path assets)
    : library_(library), assets_(fs::absolute(assets)) {}
LibraryMaintenance::~LibraryMaintenance() { stopping_ = true; if (worker_.joinable()) worker_.join(); }
nlohmann::json LibraryMaintenance::status() const { std::lock_guard lock(mutex_); return status_; }
nlohmann::json LibraryMaintenance::start(const std::string& operation, const std::vector<std::string>& selected) {
    if (operation != "verify" && operation != "rebuild" && operation != "cleanup") throw std::invalid_argument("invalid maintenance operation");
    std::lock_guard lock(mutex_);
    if (status_.value("status", "") == "running") throw std::runtime_error("maintenance already running");
    if (worker_.joinable()) worker_.join();
    for (const auto& name : selected) {
        const auto p = util::utf8_to_path(name);
        if (p.empty() || p.has_parent_path() || name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
            throw std::invalid_argument("cleanup accepts plain asset filenames only");
    }
    status_ = {{"status", "running"}, {"operation", operation}, {"checked", 0}, {"missing", 0}, {"corrupt", 0},
        {"removed", 0}, {"recovered", 0}, {"orphans", nlohmann::json::array()}, {"errors", nlohmann::json::array()}};
    worker_ = std::thread([this, operation, selected] { run(operation, selected); });
    return status_;
}
void LibraryMaintenance::run(std::string operation, std::vector<std::string> selected) {
    std::unique_lock asset_lock(library_.asset_mutex());
    auto report = status();
    try {
        std::set<fs::path> references;
        for (std::size_t offset = 0; !stopping_; offset += 500) {
            const auto page = library_.list(offset, 500);
            for (auto track : page.tracks) {
                const auto path = util::utf8_to_path(track.asset_path);
                references.insert(fs::weakly_canonical(path));
                try {
                    const auto state = !fs::is_regular_file(path) ? "missing" : hash_asset(path) == track.content_hash ? "available" : "corrupt";
                    if (library_.find_by_hash(track.content_hash)->status != state) { track.status = state; library_.update_asset(track); }
                    track.status = state;
                    report["checked"] = report["checked"].get<int>() + 1;
                    if (track.status != "available") report[track.status] = report[track.status].get<int>() + 1;
                } catch (const std::exception& e) { report["errors"].push_back({{"track_id", track.id}, {"error", e.what()}}); }
            }
            { std::lock_guard lock(mutex_); status_ = report; }
            if (offset + page.tracks.size() >= page.total) break;
        }
        for (const auto& entry : fs::directory_iterator(assets_)) {
            if (stopping_) break;
            if (entry.is_symlink() || !entry.is_regular_file() || references.contains(fs::weakly_canonical(entry.path()))) continue;
            const auto name = util::path_to_utf8(entry.path().filename());
            report["orphans"].push_back(name);
            if (operation == "cleanup" && std::find(selected.begin(), selected.end(), name) != selected.end()) {
                // Recheck current repository before each explicit deletion; imports can publish concurrently.
                bool referenced = false;
                for (std::size_t offset = 0; ; offset += 500) {
                    const auto page = library_.list(offset, 500);
                    for (const auto& t : page.tracks) if (fs::weakly_canonical(util::utf8_to_path(t.asset_path)) == fs::weakly_canonical(entry.path())) referenced = true;
                    if (offset + page.tracks.size() >= page.total) break;
                }
                if (!referenced) { fs::remove(entry.path()); report["removed"] = report["removed"].get<int>() + 1; }
            } else if (operation == "rebuild") {
                try {
                    const auto hash = hash_asset(entry.path());
                    if (library_.find_by_hash(hash)) continue;
                    const auto metadata = meta::read_metadata(entry.path());
                    if (!metadata.valid || metadata.duration.count() <= 0) throw std::runtime_error("invalid orphan audio");
                    Track track;
                    track.id = "track_" + hash.substr(7); track.content_hash = hash;
                    track.asset_path = util::path_to_utf8(entry.path()); track.title = metadata.title;
                    track.artist = metadata.artist; track.album = metadata.album; track.duration_ms = metadata.duration.count();
                    track.sample_rate = metadata.sample_rate; track.channels = metadata.channels;
                    track.format = util::path_to_utf8(entry.path().extension()).substr(1);
                    track.size_bytes = entry.file_size(); track.import_origin = "server_path"; track.source_name = name;
                    library_.register_track(track); report["recovered"] = report["recovered"].get<int>() + 1;
                } catch (const std::exception& e) { report["errors"].push_back({{"asset", name}, {"error", e.what()}}); }
            }
        }
        report["status"] = stopping_ ? "cancelled" : "completed";
    } catch (const std::exception& e) { report["status"] = "failed"; report["errors"].push_back({{"error", e.what()}}); }
    std::lock_guard lock(mutex_); status_ = report;
}
}
