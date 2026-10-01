#include "liveplay/library/track_repository.hpp"
#include "liveplay/library/store_lock.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace liveplay::library {
namespace fs = std::filesystem;
namespace {
constexpr std::uintmax_t max_catalog_bytes = 64ull * 1024 * 1024;
constexpr std::uintmax_t max_journal_bytes = 64ull * 1024 * 1024;
void validate(const Track& t) {
    const auto hash = t.content_hash;
    if (hash.size() != 71 || !hash.starts_with("sha256:") ||
        !std::all_of(hash.begin() + 7, hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        t.id != "track_" + hash.substr(7) || t.title.empty() || t.duration_ms <= 0 || t.size_bytes == 0 ||
        !util::utf8_to_path(t.asset_path).is_absolute() ||
        (t.import_origin != "server_path" && t.import_origin != "upload") ||
        (t.status != "available" && t.status != "missing" && t.status != "corrupt"))
        throw std::invalid_argument("invalid track record");
}
bool portable_child(const fs::path& relative) { return !relative.empty() && relative.is_relative() && *relative.begin() != ".."; }
}

class JsonTrackRepository::StoreState {
public:
    explicit StoreState(const fs::path& lock) : lock_(lock) {}
private:
    StoreLock lock_;
};

JsonTrackRepository::JsonTrackRepository(fs::path path, fs::path portable_root, std::function<bool()> defer_compaction)
    : path_(fs::absolute(std::move(path))), journal_path_(path_.parent_path() / "catalog.journal.jsonl"),
      portable_root_(std::move(portable_root)), state_(std::make_unique<StoreState>(path_.parent_path() / "catalog.lock")),
      defer_compaction_(std::move(defer_compaction)) {
    const auto decode = [&](Track track) {
        const auto asset = util::utf8_to_path(track.asset_path);
        if (asset.is_relative() && !portable_root_.empty()) {
            const auto relative = asset.lexically_normal();
            if (!portable_child(relative)) throw std::runtime_error("invalid relative asset path");
            track.asset_path = util::path_to_utf8(fs::absolute(portable_root_ / relative));
        }
        validate(track); return track;
    };
    if (fs::exists(path_)) {
        const auto doc = util::read_bounded_json(path_, max_catalog_bytes);
        if (!doc.is_object() || doc.at("schemaVersion") != 1 || !doc.at("tracks").is_array() || doc.at("tracks").size() > max_tracks)
            throw std::runtime_error("unsupported or invalid library catalog");
        sequence_ = doc.value("lastSequence", 0ull);
        for (const auto& value : doc.at("tracks")) {
            auto track = decode(value.get<Track>());
            if (!tracks_.emplace(track.id, track).second || !hashes_.emplace(track.content_hash, track.id).second)
                throw std::runtime_error("duplicate identity in library catalog");
        }
    } else util::atomic_write_json(path_, {{"schemaVersion",1},{"lastSequence",0},{"tracks",nlohmann::json::array()}});
    if (fs::exists(journal_path_)) {
        if (fs::file_size(journal_path_) > max_journal_bytes) throw std::runtime_error("library journal exceeds size limit");
        std::ifstream input(journal_path_, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        const auto final_newline = bytes.find_last_of('\n');
        std::size_t cursor = 0;
        while (final_newline != std::string::npos && cursor <= final_newline) {
            const auto end = bytes.find('\n', cursor);
            if (end == std::string::npos || end > final_newline) break;
            const auto line = bytes.substr(cursor, end - cursor); cursor = end + 1;
            if (line.empty()) continue;
            nlohmann::json event;
            try { event = nlohmann::json::parse(line); } catch (...) { throw std::runtime_error("invalid library journal record"); }
            if (event.value("schemaVersion", 0) != 1 || event.value("op", "") != "upsert") throw std::runtime_error("unsupported library journal record");
            const auto seq = event.at("sequence").get<std::uint64_t>();
            if (seq <= sequence_) continue;
            if (seq != sequence_ + 1) throw std::runtime_error("library journal sequence gap");
            auto track = decode(event.at("track").get<Track>());
            if (const auto hash = hashes_.find(track.content_hash); hash != hashes_.end() && hash->second != track.id) throw std::runtime_error("duplicate hash in library journal");
            if (const auto old = tracks_.find(track.id); old != tracks_.end() && old->second.content_hash != track.content_hash) throw std::runtime_error("journal changes track identity");
            tracks_[track.id] = track; hashes_[track.content_hash] = track.id; sequence_ = seq; dirty_ = true;
        }
        if (cursor < bytes.size()) util::atomic_write_text(journal_path_, bytes.substr(0, cursor));
    }
    compact_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    compactor_ = std::thread([this] { compaction_loop(); });
}

JsonTrackRepository::~JsonTrackRepository() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cv_.notify_all(); if (compactor_.joinable()) compactor_.join();
}
TrackPage JsonTrackRepository::list(std::size_t offset, std::size_t limit) const {
    std::lock_guard lock(mutex_); TrackPage page; page.total = tracks_.size(); auto it = tracks_.begin();
    std::advance(it, std::min(offset, tracks_.size())); while (it != tracks_.end() && page.tracks.size() < limit) page.tracks.push_back((it++)->second); return page;
}
std::optional<Track> JsonTrackRepository::find(const std::string& id) const { std::lock_guard lock(mutex_); const auto it = tracks_.find(id); return it == tracks_.end() ? std::nullopt : std::optional<Track>(it->second); }
std::optional<Track> JsonTrackRepository::find_by_hash(const std::string& hash) const { std::lock_guard lock(mutex_); const auto it = hashes_.find(hash); return it == hashes_.end() ? std::nullopt : std::optional<Track>(tracks_.at(it->second)); }
void JsonTrackRepository::append_upsert(const Track& track) {
    auto stored = track;
    if (!portable_root_.empty()) {
        const auto relative = util::utf8_to_path(track.asset_path).lexically_relative(portable_root_);
        if (portable_child(relative)) { const auto portable = relative.generic_u8string(); stored.asset_path = std::string(reinterpret_cast<const char*>(portable.data()), portable.size()); }
    }
    const nlohmann::json event{{"schemaVersion",1},{"sequence",sequence_ + 1},{"op","upsert"},{"track",stored}};
    if ((fs::exists(journal_path_) ? fs::file_size(journal_path_) : 0) + event.dump().size() + 1 > max_journal_bytes) compact_locked();
    util::append_durable_line(journal_path_, event.dump()); ++sequence_; dirty_ = true;
    compact_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(350); cv_.notify_one();
}
TrackInsert JsonTrackRepository::insert(const Track& track) {
    validate(track); std::lock_guard lock(mutex_);
    if (const auto it = hashes_.find(track.content_hash); it != hashes_.end()) return {tracks_.at(it->second), false};
    if (tracks_.size() >= max_tracks) throw std::runtime_error("library track limit reached");
    append_upsert(track); tracks_.emplace(track.id, track); hashes_.emplace(track.content_hash, track.id); return {track, true};
}
void JsonTrackRepository::update_asset(const Track& track) {
    validate(track); std::lock_guard lock(mutex_); const auto old = tracks_.find(track.id);
    if (old == tracks_.end() || old->second.content_hash != track.content_hash) throw std::invalid_argument("asset update cannot change identity");
    append_upsert(track); old->second = track;
}
void JsonTrackRepository::compact_locked() {
    if (!dirty_) return;
    auto values = nlohmann::json::array();
    for (const auto& [id, value] : tracks_) {
        auto stored = value;
        if (!portable_root_.empty()) { const auto relative = util::utf8_to_path(value.asset_path).lexically_relative(portable_root_); if (portable_child(relative)) { const auto p = relative.generic_u8string(); stored.asset_path = std::string(reinterpret_cast<const char*>(p.data()), p.size()); } }
        values.push_back(stored);
    }
    const nlohmann::json doc{{"schemaVersion",1},{"lastSequence",sequence_},{"tracks",values}};
    if (doc.dump(2).size() > max_catalog_bytes) throw std::runtime_error("library catalog size limit reached");
    if (fs::exists(path_)) fs::copy_file(path_, path_.parent_path() / "catalog.backup.json", fs::copy_options::overwrite_existing);
    util::atomic_write_json(path_, doc); util::atomic_write_text(journal_path_, ""); dirty_ = false;
}
void JsonTrackRepository::compaction_loop() {
    std::unique_lock lock(mutex_);
    while (true) {
        cv_.wait(lock, [this] { return stopping_ || dirty_; });
        if (stopping_) { try { compact_locked(); } catch (...) {} return; }
        const auto deadline = compact_after_;
        if (cv_.wait_until(lock, deadline, [this, deadline] { return stopping_ || compact_after_ != deadline; })) continue;
        if (defer_compaction_ && defer_compaction_()) { compact_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1); continue; }
        try { compact_locked(); } catch (...) { compact_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
    }
}
void JsonTrackRepository::checkpoint() { std::lock_guard lock(mutex_); compact_locked(); }
std::uintmax_t JsonTrackRepository::journal_bytes() const { std::lock_guard lock(mutex_); return fs::exists(journal_path_) ? fs::file_size(journal_path_) : 0; }
} // namespace liveplay::library
