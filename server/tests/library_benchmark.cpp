#include "liveplay/import/import_service.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <fstream>
#include <iostream>
using namespace liveplay;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    const auto root = fs::current_path() / ("plus-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(root / "sources");
    try {
        std::vector<importing::ImportFile> files;
        for (int n = 0; n < 1000; ++n) {
            const auto path = root / "sources" / ("track-" + std::to_string(n) + ".wav");
            std::ofstream out(path, std::ios::binary);
            const auto u16 = [&](unsigned value) { out.put(static_cast<char>(value)); out.put(static_cast<char>(value >> 8)); };
            const auto u32 = [&](unsigned value) { u16(value & 65535); u16(value >> 16); };
            out.write("RIFF", 4); u32(36 + 48000 * 4); out.write("WAVEfmt ", 8); u32(16); u16(1); u16(2);
            u32(48000); u32(192000); u16(4); u16(16); out.write("data",4); u32(192000);
            for (int i=0; i<48000; ++i) { u16(i % 100 < 50 ? n + 1 : -(n + 1)); u16(0); }
            out.close(); files.push_back({util::path_to_utf8(path), ""});
        }
        nlohmann::json results = {{"fixture", "unique 1-second PCM WAV; stereo 48kHz s16; 192044 bytes per track"},
            {"build", "Release Windows MSVC"}, {"runs", nlohmann::json::array()}};
        for (int count : {100, 500, 1000}) {
            const auto store = root / std::to_string(count);
            library::JsonTrackRepository repository(store / "library" / "catalog.json", store); library::LibraryService library(repository);
            importing::ImportService imports(library, store / "library", [](const auto&) { return true; }, 1024 * 1024, store / "assets");
            const auto start = Clock::now();
            const auto submitted = imports.submit({files.begin(), files.begin() + count}, "server_path");
            importing::ImportBatch finished;
            while (true) {
                finished = *imports.get(submitted.id);
                if (finished.status == "completed" || finished.status == "failed") break;
                if (Clock::now() - start > std::chrono::minutes(5)) throw std::runtime_error("benchmark timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            nlohmann::json run = {{"tracks", count}, {"hash_ms", 0.0}, {"copy_ms", 0.0}, {"metadata_ms", 0.0}, {"persistence_ms", 0.0},
                {"total_ms", std::chrono::duration<double, std::milli>(Clock::now() - start).count()},
                {"catalog_bytes", fs::file_size(store / "library" / "catalog.json")}, {"failed_files", 0}};
            for (const auto& result : finished.files) {
                for (const char* stage : {"hash_ms", "copy_ms", "metadata_ms", "persistence_ms"}) run[stage] = run[stage].get<double>() + result.timings[stage].get<double>();
                if (result.status != "imported") run["failed_files"] = run["failed_files"].get<int>() + 1;
            }
            run["catalog_persistence_ms"] = run["persistence_ms"];
            run["journal_persistence_ms"] = finished.journal_ms;
            run["persistence_ms"] = run["persistence_ms"].get<double>() + finished.journal_ms;
            run["other_ms"] = run["total_ms"].get<double>() - run["hash_ms"].get<double>() - run["copy_ms"].get<double>() - run["metadata_ms"].get<double>() - run["persistence_ms"].get<double>();
            results["runs"].push_back(run); std::cerr << "Completed " << count << " tracks\n";
        }
        std::cout << results.dump(2) << '\n';
        if (argc > 1) { std::ofstream out(util::utf8_to_path(argv[1])); out << results.dump(2) << '\n'; if (!out) throw std::runtime_error("cannot save benchmark results"); }
        fs::remove_all(root); // Unique fixture created only under the benchmark cwd.
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; fs::remove_all(root); return 1; }
}
