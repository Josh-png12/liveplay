#include "liveplay/import/import_service.hpp"
#include "liveplay/library/library_service.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <sodium.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace liveplay;
int plus_foundation_tests(const fs::path& root, const fs::path& wav);
namespace {
int failures = 0;
void check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
template<class F> bool throws(F action) {
    try { action(); return false; } catch (const std::exception&) { return true; }
}
void wav(const fs::path& path, int amplitude) {
    std::ofstream out(path, std::ios::binary);
    const auto u16 = [&](unsigned v) { out.put(static_cast<char>(v)); out.put(static_cast<char>(v >> 8)); };
    const auto u32 = [&](unsigned v) { u16(v & 65535); u16(v >> 16); };
    out.write("RIFF", 4); u32(36 + 48000 * 4);
    out.write("WAVEfmt ", 8); u32(16); u16(1); u16(2);
    u32(48000); u32(48000 * 4); u16(4); u16(16);
    out.write("data", 4); u32(48000 * 4);
    for (int i = 0; i < 48000; ++i) { u16(i % 100 < 50 ? amplitude : -amplitude); u16(0); }
}
importing::ImportBatch wait(importing::ImportService& service, const std::string& id) {
    for (int i = 0; i < 500; ++i) {
        const auto value = *service.get(id);
        if (value.status == "completed" || value.status == "cancelled" || value.status == "failed") return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("batch timed out");
}
library::Track record(const fs::path& root, char digit) {
    library::Track t;
    t.content_hash = "sha256:" + std::string(64, digit);
    t.id = "track_" + std::string(64, digit);
    t.title = "Example"; t.duration_ms = 1000; t.format = "wav";
    t.asset_path = util::path_to_utf8(root / (std::string(1, digit) + ".wav"));
    t.size_bytes = 100; t.import_origin = "server_path";
    t.source_name = "Example.wav"; t.imported_at = "2026-09-30T00:00:00Z";
    return t;
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (sodium_init() < 0) return 1;
    const auto root = fs::current_path() / util::utf8_to_path("library-test-árbol") /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    try {
        const auto catalog = root / "repository.json";
        {
            library::JsonTrackRepository repository(catalog);
            library::LibraryService service(repository);
            check(service.list().total == 0, "missing catalog starts empty");
            const auto t = record(root, 'a');
            check(service.register_track(t).inserted, "first insert is durable");
            auto renamed = t; renamed.title = "Changed";
            check(!service.register_track(renamed).inserted && service.find(t.id)->title == "Example",
                  "duplicate content preserves identity and original metadata");
            std::atomic<int> inserted{0};
            std::vector<std::thread> threads;
            for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
                if (service.register_track(record(root, 'b')).inserted) ++inserted;
            });
            for (auto& thread : threads) thread.join();
            check(inserted == 1 && service.list().total == 2, "concurrent uniqueness by hash");
            check(service.list(1, 1).tracks.size() == 1 && service.list(999).tracks.empty(), "pagination bounds");
            check(throws([&] { service.list(0, 501); }), "reject oversized page");
#if defined(_WIN32)
            HANDLE locked = CreateFileW(catalog.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(locked != INVALID_HANDLE_VALUE, "lock destination fixture");
            check(throws([&] { service.register_track(record(root, 'c')); }), "Windows locked catalog reports write failure");
            check(service.list().total == 2, "failed commit does not publish in memory");
            if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
#endif
        }
        {
            library::JsonTrackRepository repository(catalog);
            check(repository.list(0, 100).total == 2, "repeated atomic replacement persists and reloads");
            auto invalid = record(root, 'd'); invalid.content_hash = "invalid";
            check(throws([&] { repository.insert(invalid); }), "invalid track rejected");
        }
        util::atomic_write_json(root / "corrupt.json", nlohmann::json{{"schemaVersion", 9}, {"tracks", nlohmann::json::array()}});
        check(throws([&] { library::JsonTrackRepository bad(root / "corrupt.json"); }), "unknown schema rejected without overwrite");
        {
            std::ofstream broken(root / "broken.json"); broken << "{"; broken.close();
            check(throws([&] { library::JsonTrackRepository bad(root / "broken.json"); }), "corrupt JSON fails closed for library");
        }
        wav(root / "same.wav", 1000);
        fs::copy_file(root / "same.wav", root / util::utf8_to_path("otro nombre.wav"));
        wav(root / "different.wav", 2000);
        std::ofstream(root / "bad.wav") << "not audio";
        library::JsonTrackRepository repository(root / "library" / "catalog.json");
        library::LibraryService library(repository);
        {
            importing::ImportService imports(library, root / "library", [](const auto&) { return true; }, 1024 * 1024);
            std::vector<importing::ImportFile> files;
            for (const auto& name : {"same.wav", "otro nombre.wav", "different.wav", "bad.wav", "missing.wav"})
                files.push_back({util::path_to_utf8(root / name), {}});
            const auto submitted = imports.submit(files, "server_path", "fixture-request");
            check(imports.submit(files, "server_path", "fixture-request").id == submitted.id, "requestId retry is idempotent");
            check(throws([&] { imports.submit({files.front()}, "server_path", "fixture-request"); }), "requestId rejects different inputs");
            const auto result = wait(imports, submitted.id);
            check(result.status == "completed" && result.files[0].status == "imported" &&
                  result.files[1].status == "duplicate" && result.files[2].status == "imported", "multi-file import deduplicates exact bytes");
            check(result.files[3].status == "failed" && result.files[4].status == "failed" && library.list().total == 2,
                  "bad/missing files do not abort valid files");
            const auto t = *library.find(result.files[0].track_id);
            check(t.title == "same" && t.duration_ms == 1000 && t.sample_rate == 48000 && t.channels == 2,
                  "shared metadata extractor supplies title duration rate channels");
            check(t.content_hash.starts_with("sha256:") && t.content_hash.size() == 71 &&
                  fs::exists(util::utf8_to_path(t.asset_path)) && t.asset_path != files[0].path,
                  "content hash and immutable owned asset stored");
            check(throws([&] { imports.stage_upload("../escape.wav", "abc"); }), "upload traversal rejected");
            check(throws([&] { imports.stage_upload("stream:ads.wav", "abc"); }), "upload alternate stream rejected");
            check(throws([&] { imports.stage_upload("empty.wav", ""); }), "empty upload rejected");
            const auto staging = imports.stage_upload("too-big.wav", std::string(16, 'a'));
            auto uploaded = wait(imports, imports.submit({{util::path_to_utf8(staging), {}}}, "upload").id);
            check(uploaded.files[0].status == "failed" && !fs::exists(staging), "failed uploaded file is cleaned");
        }
        {
            importing::ImportService denied(library, root / "denied", [](const auto&) { return false; }, 1024);
            check(throws([&] { denied.submit({{util::path_to_utf8(root / "same.wav"), {}}}, "server_path"); }),
                  "service enforces source path policy independently of HTTP");
        }
        {
            importing::ImportService limited(library, root / "limited", [](const auto&) { return true; }, 1024);
            const auto result = wait(limited, limited.submit({{util::path_to_utf8(root / "same.wav"), {}}}, "server_path").id);
            check(result.files[0].status == "failed", "asset size limit enforced in worker");
        }
        const auto resume_root = root / "resume";
        fs::create_directories(resume_root / "imports");
        importing::ImportBatch interrupted;
        interrupted.id = "batch_" + std::string(32, 'a');
        interrupted.origin = "server_path"; interrupted.status = "running";
        interrupted.files.push_back({{util::path_to_utf8(root / "same.wav"), "same.wav"}});
        util::atomic_write_json(resume_root / "imports" / (interrupted.id + ".json"), nlohmann::json(interrupted));
        {
            importing::ImportService resumed(library, resume_root, [](const auto&) { return true; }, 1024 * 1024);
            const auto result = wait(resumed, interrupted.id);
            check(result.status == "completed" && result.files[0].status == "duplicate", "crashed running batch resumes pending file without duplicate track");
        }
        interrupted.id = "batch_" + std::string(32, 'b');
        interrupted.cancel_requested = true;
        util::atomic_write_json(resume_root / "imports" / (interrupted.id + ".json"), nlohmann::json(interrupted));
        {
            importing::ImportService resumed(library, resume_root, [](const auto&) { return true; }, 1024 * 1024);
            const auto result = wait(resumed, interrupted.id);
            check(result.status == "cancelled" && result.files[0].status == "cancelled", "durable cancellation survives restart");
        }
        check(library.list().total == 2, "all imports are library-only; no engine dependency");
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what()); ++failures;
    }
    failures += plus_foundation_tests(root / "foundation", root / "same.wav");
    // Root is a uniquely generated test directory under the build working dir.
    fs::remove_all(root);
    std::printf("library failures: %d\n", failures);
    return failures ? 1 : 0;
}
