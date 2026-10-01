#pragma once

#include "liveplay/library/library_service.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>

namespace liveplay::importing {

struct ImportFile {
    std::string path;
    std::string name;
};
struct ImportResult {
    ImportFile source;
    std::string status = "queued"; // imported, duplicate, failed, cancelled
    std::string track_id;
    std::string error;
    nlohmann::json timings = {{"hash_ms", 0.0}, {"copy_ms", 0.0}, {"metadata_ms", 0.0}, {"persistence_ms", 0.0}};
};
struct ImportBatch {
    std::string id;
    std::string request_id;
    std::string origin;
    std::string status = "queued"; // running, completed, cancelled
    bool cancel_requested = false;
    std::string relink_track_id;
    double journal_ms = 0;
    std::vector<ImportResult> files;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ImportFile, path, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ImportResult, source, status, track_id, error, timings)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ImportBatch, id, request_id, origin, status,
    cancel_requested, files, relink_track_id, journal_ms)

class ImportService {
public:
    using PathPolicy = std::function<bool(const std::filesystem::path&)>;
    static constexpr std::size_t max_files = 1000;
    static constexpr std::size_t max_batches = 64;
    ImportService(library::LibraryService& library, std::filesystem::path root,
                  PathPolicy policy, std::uint64_t max_asset_bytes, std::filesystem::path assets = {});
    ~ImportService();
    // Journals the batch before accepting; same request ID + inputs is idempotent.
    ImportBatch submit(std::vector<ImportFile> files, const std::string& origin,
                       const std::string& request_id = "", const std::string& relink_track_id = "");
    std::optional<ImportBatch> get(const std::string& id) const;
    std::optional<ImportBatch> cancel(const std::string& id);
    // Multipart bytes are staged here, with no dependence on an open project.
    std::filesystem::path stage_upload(const std::string& filename,
                                      const std::string& bytes);
    void discard_upload(const std::filesystem::path& path) noexcept;
private:
    library::LibraryService& library_;
    std::filesystem::path root_;
    std::filesystem::path assets_;
    PathPolicy policy_;
    std::uint64_t max_asset_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, ImportBatch> batches_;
    std::deque<std::string> queue_;
    bool stopping_ = false;
    std::thread worker_;
    void run();
    library::TrackInsert import_one(const ImportFile& file, const std::string& origin,
                                   const std::string& batch_id, std::size_t index, nlohmann::json& timings);
    bool interrupted(const std::string& id) const;
    double persist(const ImportBatch& batch) const;
};

} // namespace liveplay::importing
