#include "liveplay/import/import_service.hpp"
#include "liveplay/meta/metadata.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace liveplay::importing {
namespace fs = std::filesystem;
namespace {
std::string hex(const unsigned char* bytes, std::size_t size) {
    std::string result(size * 2, '0');
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) {
        result[2 * i] = digits[bytes[i] >> 4];
        result[2 * i + 1] = digits[bytes[i] & 15];
    }
    return result;
}
std::string random_id() {
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    return hex(bytes.data(), bytes.size());
}
std::string utc_now() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}
std::string extension(const fs::path& p) {
    auto ext = util::path_to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    static const std::array supported{".wav", ".mp3", ".flac", ".ogg", ".m4a",
        ".aac", ".aif", ".aiff", ".opus", ".wma"};
    if (std::find(supported.begin(), supported.end(), ext) == supported.end())
        throw std::invalid_argument("unsupported audio file extension");
    return ext;
}
bool valid_id(const std::string& id) {
    return id.size() == 38 && id.starts_with("batch_") &&
        std::all_of(id.begin() + 6, id.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
bool terminal(const ImportBatch& batch) {
    return batch.status == "completed" || batch.status == "cancelled";
}
bool within(const fs::path& path, const fs::path& root) {
    // Component comparison avoids root-prefix lookalikes and resolves symlinks.
    const auto p = fs::weakly_canonical(path);
    const auto r = fs::weakly_canonical(root);
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
        if (pi == p.end() || *pi != *ri) return false;
    }
    return pi != p.end();
}
}

ImportService::ImportService(library::LibraryService& library, fs::path root,
                             PathPolicy policy, std::uint64_t max_asset_bytes, fs::path assets)
    : library_(library), root_(fs::absolute(std::move(root))),
      assets_(assets.empty() ? root_ / "assets" : fs::absolute(assets)),
      policy_(std::move(policy)), max_asset_bytes_(max_asset_bytes) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialization failed");
    fs::create_directories(assets_);
    fs::create_directories(root_ / "staging");
    fs::create_directories(root_ / "imports");
    fs::create_directories(root_ / "uploads");
    for (const auto& entry : fs::directory_iterator(root_ / "imports")) {
        if (entry.path().extension() != ".json") continue;
        if (batches_.size() >= max_batches) throw std::runtime_error("too many import journals");
        auto batch = util::read_bounded_json(entry.path(), 8ull * 1024 * 1024).get<ImportBatch>();
        if (!valid_id(batch.id) || entry.path().stem() != batch.id ||
            batch.files.empty() || batch.files.size() > max_files ||
            (batch.origin != "server_path" && batch.origin != "upload") ||
            (batch.status != "queued" && batch.status != "running" && !terminal(batch)))
            throw std::runtime_error("invalid import journal");
        for (auto& file : batch.files) {
            if (batch.origin == "upload" && util::utf8_to_path(file.source.path).is_relative()) {
                const auto relative = util::utf8_to_path(file.source.path).lexically_normal();
                if (relative.empty() || *relative.begin() != "uploads") throw std::runtime_error("invalid portable upload path");
                const auto resolved = root_ / relative;
                if (!within(resolved, root_ / "uploads")) throw std::runtime_error("invalid portable upload path");
                file.source.path = util::path_to_utf8(resolved);
            }
            if (file.status != "queued" && file.status != "imported" &&
                file.status != "duplicate" && file.status != "failed" && file.status != "cancelled")
                throw std::runtime_error("invalid import result");
        }
        if (!terminal(batch)) {
            batch.status = "queued"; // Crash recovery: retry pending files only.
            batch.journal_ms += persist(batch);
            queue_.push_back(batch.id);
        }
        if (!batches_.emplace(batch.id, batch).second)
            throw std::runtime_error("duplicate import journal");
    }
    worker_ = std::thread([this] { run(); });
}
ImportService::~ImportService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}
double ImportService::persist(const ImportBatch& batch) const {
    const auto start = std::chrono::steady_clock::now();
    auto stored = batch;
    if (stored.origin == "upload") for (auto& file : stored.files) {
        const auto relative = util::utf8_to_path(file.source.path).lexically_relative(root_);
        if (!relative.empty() && *relative.begin() != "..") {
            const auto portable = relative.generic_u8string();
            file.source.path = std::string(reinterpret_cast<const char*>(portable.data()), portable.size());
        }
    }
    const nlohmann::json doc = stored;
    if (doc.dump(2).size() > 8ull * 1024 * 1024)
        throw std::length_error("import journal exceeds size limit");
    util::atomic_write_json(root_ / "imports" / (batch.id + ".json"), doc);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

ImportBatch ImportService::submit(std::vector<ImportFile> files, const std::string& origin,
                                  const std::string& request_id, const std::string& relink_track_id) {
    if (files.empty() || files.size() > max_files)
        throw std::invalid_argument("files must contain 1..1000 entries");
    if ((origin != "server_path" && origin != "upload") || request_id.size() > 128)
        throw std::invalid_argument("invalid import origin or requestId");
    if (!relink_track_id.empty() && (files.size() != 1 || !library_.find(relink_track_id)))
        throw std::invalid_argument("relink requires one file and an existing track");
    for (auto& file : files) {
        if (file.path.empty() || file.path.size() > 32768 || file.path.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid source path");
        const auto source = util::utf8_to_path(file.path);
        if (!source.is_absolute()) throw std::invalid_argument("source paths must be absolute");
        if (origin == "server_path" && !policy_(source))
            throw std::invalid_argument("source path denied by filesystem policy");
        if (origin == "upload" && !within(source, root_ / "uploads"))
            throw std::invalid_argument("invalid upload staging path");
        file.path = util::path_to_utf8(fs::weakly_canonical(source));
        file.name = util::path_to_utf8(source.filename());
    }
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("import service stopping");
    if (!request_id.empty()) {
        for (const auto& [id, previous] : batches_) {
            if (previous.request_id != request_id) continue;
            std::vector<ImportFile> previous_files;
            for (const auto& result : previous.files) previous_files.push_back(result.source);
            if (previous.origin != origin || previous.relink_track_id != relink_track_id || nlohmann::json(previous_files) != nlohmann::json(files))
                throw std::invalid_argument("requestId already used for different inputs");
            return previous;
        }
    }
    // Bounded history; expired request IDs may create a new batch but hashes still
    // prevent duplicate tracks. Never evict a queued/running batch.
    if (batches_.size() >= max_batches) {
        const auto old = std::find_if(batches_.begin(), batches_.end(), [](const auto& entry) {
            return terminal(entry.second);
        });
        if (old == batches_.end()) throw std::length_error("import queue is full");
        fs::remove(root_ / "imports" / (old->first + ".json"));
        batches_.erase(old);
    }
    ImportBatch batch;
    batch.id = "batch_" + random_id();
    batch.request_id = request_id;
    batch.relink_track_id = relink_track_id;
    batch.origin = origin;
    for (const auto& file : files) batch.files.push_back(ImportResult{file});
    batch.journal_ms += persist(batch);
    batches_.emplace(batch.id, batch);
    queue_.push_back(batch.id);
    cv_.notify_one();
    return batch;
}
std::optional<ImportBatch> ImportService::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = batches_.find(id);
    return it == batches_.end() ? std::nullopt : std::optional<ImportBatch>(it->second);
}
std::optional<ImportBatch> ImportService::cancel(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = batches_.find(id);
    if (it == batches_.end()) return std::nullopt;
    auto next = it->second;
    if (!terminal(next)) {
        next.cancel_requested = true;
        next.journal_ms += persist(next);
        it->second = next;
    }
    return next;
}
bool ImportService::interrupted(const std::string& id) const {
    std::lock_guard lock(mutex_);
    return stopping_ || batches_.at(id).cancel_requested;
}

fs::path ImportService::stage_upload(const std::string& filename, const std::string& bytes) {
    if (filename.empty() || filename.size() > 255 || filename == "." || filename == ".." ||
        filename.find_first_of("/\\:\0", 0, 4) != std::string::npos)
        throw std::invalid_argument("upload filename must be a plain filename");
    extension(util::utf8_to_path(filename));
    if (bytes.empty() || bytes.size() > max_asset_bytes_)
        throw std::invalid_argument("upload is empty or exceeds asset size limit");
    const auto folder = root_ / "uploads" / random_id();
    fs::create_directory(folder);
    const auto dest = folder / util::utf8_to_path(filename);
    try {
        std::ofstream out(dest, std::ios::binary);
        if (!out) throw std::runtime_error("cannot stage upload");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("cannot write upload");
        return dest;
    } catch (...) {
        discard_upload(dest);
        throw;
    }
}
void ImportService::discard_upload(const fs::path& path) noexcept {
    try {
        if (!within(path, root_ / "uploads")) return;
        std::error_code ec;
        fs::remove(path, ec);
        // Only remove the now-empty per-upload folder, never recursively delete.
        fs::remove(path.parent_path(), ec);
    } catch (...) {}
}

library::TrackInsert ImportService::import_one(const ImportFile& file, const std::string& origin,
                                               const std::string& batch_id, std::size_t index, nlohmann::json& timings) {
    std::unique_lock asset_lock(library_.asset_mutex());
    const auto source = util::utf8_to_path(file.path);
    if ((origin == "server_path" && !policy_(source)) ||
        (origin == "upload" && !within(source, root_ / "uploads")))
        throw std::runtime_error("source path denied by filesystem policy");
    if (!fs::is_regular_file(source)) throw std::runtime_error("source is not a regular file");
    const auto ext = extension(source);
    const auto source_size = fs::file_size(source);
    if (source_size == 0 || source_size > max_asset_bytes_)
        throw std::runtime_error("source is empty or exceeds asset size limit");
    const auto stage = root_ / "staging" / (batch_id + "_" + std::to_string(index) + ext);
    try {
        crypto_hash_sha256_state hash_state{};
        crypto_hash_sha256_init(&hash_state);
        using Clock = std::chrono::steady_clock;
        const auto elapsed = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
        std::uint64_t copied = 0;
        {
            std::ifstream in(source, std::ios::binary);
            std::ofstream out(stage, std::ios::binary | std::ios::trunc);
            if (!in || !out) throw std::runtime_error("cannot open source or staging file");
            std::array<char, 64 * 1024> buffer{};
            while (in) {
                if (interrupted(batch_id)) throw std::runtime_error("import interrupted");
                auto stamp = Clock::now();
                in.read(buffer.data(), buffer.size());
                const auto count = in.gcount();
                copied += static_cast<std::uint64_t>(count);
                if (copied > max_asset_bytes_) throw std::runtime_error("asset size limit exceeded");
                out.write(buffer.data(), count);
                if (!out) throw std::runtime_error("cannot write staged asset");
                timings["copy_ms"] = timings["copy_ms"].get<double>() + elapsed(stamp);
                stamp = Clock::now();
                crypto_hash_sha256_update(&hash_state,
                    reinterpret_cast<const unsigned char*>(buffer.data()),
                    static_cast<unsigned long long>(count));
                timings["hash_ms"] = timings["hash_ms"].get<double>() + elapsed(stamp);
            }
            if (!in.eof() || copied != source_size)
                throw std::runtime_error("source read failed or changed size during import");
            out.close();
            if (!out) throw std::runtime_error("cannot finish staged asset");
        }
        std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
        crypto_hash_sha256_final(&hash_state, digest.data());
        const auto content_hash = "sha256:" + hex(digest.data(), digest.size());
        std::string relink_id;
        { std::lock_guard lock(mutex_); relink_id = batches_.at(batch_id).relink_track_id; }
        if (!relink_id.empty()) {
            auto previous = library_.find(relink_id);
            if (!previous || previous->content_hash != content_hash) throw std::runtime_error("relink SHA-256 mismatch");
            if (interrupted(batch_id)) throw std::runtime_error("import interrupted");
            const auto asset = assets_ / (content_hash.substr(7) + ext);
            // Explicit relink is authorized replacement of this owned asset only.
            if (fs::exists(asset)) fs::remove(asset);
            fs::rename(stage, asset);
            previous->asset_path = util::path_to_utf8(asset); previous->status = "available";
            const auto stamp = Clock::now(); library_.update_asset(*previous);
            timings["persistence_ms"] = elapsed(stamp);
            return {*previous, true};
        }
        if (const auto previous = library_.find_by_hash(content_hash)) {
            if (!fs::is_regular_file(util::utf8_to_path(previous->asset_path)))
                throw std::runtime_error("duplicate track asset is missing; repair required");
            fs::remove(stage);
            return {*previous, false};
        }
        auto stamp = Clock::now();
        const auto metadata = meta::read_metadata(stage);
        timings["metadata_ms"] = elapsed(stamp);
        if (!metadata.valid || metadata.duration.count() <= 0)
            throw std::runtime_error("cannot read valid audio metadata");
        if (interrupted(batch_id)) throw std::runtime_error("import interrupted");
        const auto asset = assets_ / (content_hash.substr(7) + ext);
        // Recover an orphan from a crash before catalog commit by replacing it
        // with the freshly hashed copy. No published track uses this hash yet.
        if (fs::exists(asset)) fs::remove(asset);
        fs::rename(stage, asset);
        library::Track track;
        track.id = "track_" + content_hash.substr(7);
        track.source_name = file.name;
        track.title = metadata.title == util::path_to_utf8(stage.stem())
            ? util::path_to_utf8(source.stem()) : metadata.title;
        track.artist = metadata.artist;
        track.album = metadata.album;
        track.duration_ms = metadata.duration.count();
        track.format = ext.substr(1);
        track.sample_rate = metadata.sample_rate;
        track.channels = metadata.channels;
        track.asset_path = util::path_to_utf8(asset);
        track.content_hash = content_hash;
        track.size_bytes = copied;
        track.import_origin = origin;
        track.imported_at = utc_now();
        try {
            stamp = Clock::now();
            const auto registered = library_.register_track(track);
            timings["persistence_ms"] = elapsed(stamp);
            return registered;
        } catch (...) {
            std::error_code ec;
            fs::remove(asset, ec); // Catalog failure never publishes a ready track.
            throw;
        }
    } catch (...) {
        std::error_code ec;
        fs::remove(stage, ec);
        throw;
    }
}

void ImportService::run() {
    while (true) {
        std::string id;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            id = queue_.front();
            queue_.pop_front();
        }
        try {
            std::size_t count;
            std::string origin;
            {
                std::lock_guard lock(mutex_);
                auto next = batches_.at(id);
                next.status = "running";
                next.journal_ms += persist(next);
                batches_.at(id) = next;
                count = next.files.size();
                origin = next.origin;
            }
            for (std::size_t i = 0; i < count; ++i) {
                ImportResult result;
                bool cancelled = false;
                std::vector<fs::path> cancelled_uploads;
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_) break;
                    if (batches_.at(id).cancel_requested) {
                        auto next = batches_.at(id);
                        for (auto& pending : next.files) {
                            if (pending.status != "queued") continue;
                            pending.status = "cancelled";
                            if (origin == "upload") cancelled_uploads.push_back(util::utf8_to_path(pending.source.path));
                        }
                        next.journal_ms += persist(next);
                        batches_.at(id) = next;
                        cancelled = true;
                    }
                    result = batches_.at(id).files[i];
                    if (!cancelled && result.status != "queued") continue;
                }
                if (cancelled) {
                    for (const auto& path : cancelled_uploads) discard_upload(path);
                    break;
                }
                try {
                    if (interrupted(id)) throw std::runtime_error("import interrupted");
                    const auto registered = import_one(result.source, origin, id, i, result.timings);
                    result.track_id = registered.track.id;
                    result.status = registered.inserted ? "imported" : "duplicate";
                } catch (const std::exception& error) {
                    result.status = "failed";
                    result.error = error.what();
                }
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_ && result.track_id.empty()) break;
                    auto next = batches_.at(id);
                    if (next.cancel_requested && result.track_id.empty()) {
                        result.status = "cancelled";
                        result.error.clear();
                    }
                    next.files[i] = result;
                    next.journal_ms += persist(next);
                    batches_.at(id) = next;
                }
                if (origin == "upload") discard_upload(util::utf8_to_path(result.source.path));
            }
            {
                std::lock_guard lock(mutex_);
                auto next = batches_.at(id);
                next.status = stopping_ ? "queued" : next.cancel_requested ? "cancelled" : "completed";
                next.journal_ms += persist(next);
                batches_.at(id) = next;
            }
        } catch (const std::exception& error) {
            // A journal failure stops ingestion instead of accepting more writes
            // that cannot be recovered. Existing playback is entirely independent.
            std::lock_guard lock(mutex_);
            for (auto& [batch_id, batch] : batches_) {
                if (terminal(batch)) continue;
                batch.status = "failed";
                for (auto& result : batch.files) {
                    if (result.status == "queued") { result.status = "failed"; result.error = error.what(); }
                }
            }
            stopping_ = true;
            return;
        }
    }
}

} // namespace liveplay::importing
