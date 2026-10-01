#include "liveplay/library/data_directory.hpp"
#include "liveplay/library/maintenance.hpp"
#include "liveplay/import/import_service.hpp"
#include "liveplay/analysis/analysis.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <cstdio>
#include <fstream>
namespace fs = std::filesystem;
using namespace liveplay;
namespace {
class FailingProvider final : public analysis::AnalysisProvider {
public:
    std::string id() const override { return "failing_test"; }
    nlohmann::json capabilities() const override { return nlohmann::json::object(); }
    nlohmann::json execute(const analysis::AnalysisJob&) override { throw 42; }
};
}
int plus_foundation_tests(const fs::path& root, const fs::path& wav) {
    int failures = 0;
    const auto check = [&](bool value, const char* text) { std::printf("%s %s\n", value ? "PASS" : "FAIL", text); if (!value) ++failures; };
    const auto throws = [](auto fn) { try { fn(); return false; } catch (...) { return true; } };
    const auto maintenance_done = [](library::LibraryMaintenance& maintenance) {
        for (int i = 0; i < 1000; ++i) { auto status = maintenance.status(); if (status.at("status") != "running") return status; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        throw std::runtime_error("maintenance timeout");
    };
    const auto batch_done = [](importing::ImportService& service, const std::string& id) {
        for (int i = 0; i < 1000; ++i) { auto batch = *service.get(id); if (batch.status == "completed" || batch.status == "failed") return batch; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        throw std::runtime_error("relink timeout");
    };
    try {
        const auto configured = library::resolve_plus_directory(util::path_to_utf8(root / "configured"), [](auto) { return std::string{}; });
        check(configured == root / "configured", "portable configuration override");
        const auto environment = library::resolve_plus_directory("ignored", [&](const char* key) { return std::string(key) == "LIVEPLAY_PLUS_DATA_DIR" ? util::path_to_utf8(root / "environment") : ""; });
        check(environment == root / "environment", "environment overrides portable configuration");
#if defined(_WIN32)
        const auto local = library::resolve_plus_directory("", [&](const char* key) { return std::string(key) == "LOCALAPPDATA" ? util::path_to_utf8(root) : ""; });
        check(local == root / "LivePlay" / "plus", "Windows LocalAppData layout");
#endif
        const auto legacy = root / "legacy";
        fs::create_directories(legacy / "assets"); fs::copy_file(wav, legacy / "assets" / "music.wav");
        library::Track track;
        track.content_hash = library::hash_asset(wav); track.id = "track_" + track.content_hash.substr(7);
        track.title = "Original metadata"; track.duration_ms = 1000; track.format = "wav"; track.size_bytes = fs::file_size(wav);
        track.import_origin = "server_path"; track.asset_path = util::path_to_utf8(legacy / "assets" / "music.wav");
        { library::JsonTrackRepository repo(legacy / "catalog.json"); repo.insert(track); }
        const auto destination = root / "plus";
        check(library::migrate_plus_directory(legacy, destination), "legacy migration copies into Plus layout");
        check(fs::exists(legacy / "catalog.json") && fs::exists(destination / "assets" / "music.wav"), "migration preserves old catalog and assets");
        check(!library::migrate_plus_directory(legacy, destination), "migration is idempotent and never overwrites new catalog");
        const auto stored = util::read_bounded_json(destination / "library" / "catalog.json", 1000000);
        check(stored["tracks"][0]["asset_path"] == "assets/music.wav", "catalog stores portable relative asset paths");
        const auto moved = root / "portable copy";
        fs::copy(destination, moved, fs::copy_options::recursive);
        library::JsonTrackRepository repository(moved / "library" / "catalog.json", moved);
        library::LibraryService library(repository);
        const auto moved_asset = moved / "assets" / "music.wav";
        check(library.find(track.id)->asset_path == util::path_to_utf8(moved_asset), "moving Plus directory resolves assets at new root");
        fs::remove(moved_asset);
        check(library.find(track.id)->status == "missing" && library.find(track.id)->title == track.title, "missing state preserves metadata");
        library::LibraryMaintenance maintenance(library, moved / "assets");
        maintenance.start("verify"); check(maintenance_done(maintenance)["missing"] == 1, "verification detects and persists missing asset");
        importing::ImportService imports(library, moved / "library", [](auto) { return true; }, 1000000, moved / "assets");
        const auto batch = imports.submit({{util::path_to_utf8(wav), ""}}, "server_path", "", track.id);
        check(batch_done(imports, batch.id).files[0].status == "imported" && library.find(track.id)->status == "available", "explicit relink restores verified bytes without changing track ID");
        const auto asset = util::utf8_to_path(library.find(track.id)->asset_path);
        { std::fstream out(asset, std::ios::binary | std::ios::in | std::ios::out); out.seekp(44); out.put('x'); }
        maintenance.start("verify"); check(maintenance_done(maintenance)["corrupt"] == 1, "SHA-256 verification detects asset corruption");
        const auto bad_relink = imports.submit({{util::path_to_utf8(asset), ""}}, "server_path", "", track.id);
        check(batch_done(imports, bad_relink.id).files[0].status == "failed", "relink rejects different content hash");
        const auto repaired = imports.submit({{util::path_to_utf8(wav), ""}}, "server_path", "", track.id);
        check(batch_done(imports, repaired.id).files[0].status == "imported", "relink repairs corrupt owned asset");
        fs::copy_file(wav, moved / "assets" / "orphan.wav");
        { std::fstream out(moved / "assets" / "orphan.wav", std::ios::binary | std::ios::in | std::ios::out); out.seekp(44); out.put('z'); }
        maintenance.start("verify"); check(maintenance_done(maintenance)["orphans"].size() == 1 && fs::exists(moved / "assets" / "orphan.wav"), "orphan detection is non-destructive");
        maintenance.start("rebuild"); check(maintenance_done(maintenance)["recovered"] == 1 && library.list().total == 2, "explicit rebuild recovers unindexed audio metadata");
        std::ofstream(moved / "assets" / "unused.bin") << "orphan";
        maintenance.start("cleanup", {"unused.bin", util::path_to_utf8(asset.filename())});
        check(maintenance_done(maintenance)["removed"] == 1 && fs::exists(asset), "explicit selected cleanup never deletes referenced assets");
        check(throws([&] { maintenance.start("cleanup", {"../catalog.json"}); }), "cleanup rejects path traversal");
        const auto analysis_dir = root / "analysis";
        analysis::JsonTrackAnalysisRepository analyses(analysis_dir / "tracks");
        analysis::AnalysisJob protocol_job; protocol_job.id = "job_test"; protocol_job.track_id = track.id; protocol_job.content_hash = track.content_hash;
        auto protocol_record = analysis::empty_analysis(track.id); protocol_record["status"] = "completed";
        const nlohmann::json response = {{"protocol_version",1},{"job_id",protocol_job.id},{"content_hash",track.content_hash},{"analysis",protocol_record}};
        check(analysis::worker_request(protocol_job)["protocol_version"] == 1 && analysis::worker_response(protocol_job,response)["track_id"] == track.id, "external worker JSON protocol validates correlation and nullable schema");
        auto stale = response; stale["content_hash"] = "other";
        check(throws([&] { analysis::worker_response(protocol_job,stale); }), "external worker stale output is rejected");
        {
            analysis::AnalysisScheduler scheduler(analyses, analysis_dir);
            check(scheduler.jobs().empty() && !scheduler.capabilities()["basic"].get<bool>(), "empty scheduler advertises no analysis providers");
            check(!analyses.find(track.id), "analysis can be absent independently of library");
            scheduler.enqueue(*library.find(track.id), "unavailable");
            for (int i = 0; i < 500 && scheduler.jobs()[0]["status"] != "failed"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            check(scheduler.jobs()[0]["status"] == "failed" && library.list().total == 2, "analysis failure leaves library/playback data intact");
            const auto result = analyses.find(track.id);
            check(result->at("basic")["peak_dbfs"].is_null() && result->at("rhythm")["bpm"].is_null(), "failed analysis keeps analytical values null");
            scheduler.stop(); check(throws([&] { scheduler.enqueue(track, "test"); }) && library.find(track.id).has_value(), "stopped worker cannot prevent track access");
        }
        auto jobs = util::read_bounded_json(analysis_dir / "jobs.json", 1000000); jobs["jobs"][0]["status"] = "running";
        util::atomic_write_json(analysis_dir / "jobs.json", jobs);
        { analysis::AnalysisScheduler resumed(analyses, analysis_dir);
          for (int i=0; i<500 && resumed.jobs()[0]["status"] != "failed"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
          check(resumed.jobs()[0]["status"] == "failed", "analysis running job recovers after restart and reports unavailable provider"); }
        {
            analysis::AnalysisScheduler failure(analyses, root / "provider-failure", {std::make_shared<FailingProvider>()});
            failure.enqueue(*library.find(track.id), "failing_test");
            for (int i=0; i<500 && failure.jobs()[0]["status"] != "failed"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            check(failure.jobs()[0]["status"] == "failed" && library.find(track.id).has_value(), "non-standard provider exception is isolated from library and playback");
        }
        const auto catalog = moved / "library" / "catalog.json";
        std::ofstream(catalog) << "{";
        library::backup_corrupt_catalog(catalog);
        check(throws([&] { library::JsonTrackRepository corrupt(catalog, moved); }) && fs::exists(asset), "corrupt catalog preserves assets and requires explicit recovery");
        bool backup = false; for (const auto& entry : fs::directory_iterator(catalog.parent_path())) if (entry.path().filename().string().starts_with("catalog.corrupt.")) backup = true;
        check(backup, "corrupt catalog backup retained for diagnosis");
    } catch (const std::exception& e) { std::printf("FAIL Plus foundation exception: %s\n", e.what()); ++failures; }
    return failures;
}
