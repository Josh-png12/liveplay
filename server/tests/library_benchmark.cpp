#include "liveplay/import/import_service.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <fstream>
#include <iostream>
#include <atomic>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
using namespace liveplay;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    const auto root = fs::current_path() / ("plus-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(root / "sources");
    try {
        std::vector<importing::ImportFile> files;
        for (int n = 0; n < 5000; ++n) {
            const auto path = root / "sources" / ("track-" + std::to_string(n) + ".wav");
            std::ofstream out(path, std::ios::binary);
            const auto u16 = [&](unsigned value) { out.put(static_cast<char>(value)); out.put(static_cast<char>(value >> 8)); };
            const auto u32 = [&](unsigned value) { u16(value & 65535); u16(value >> 16); };
            out.write("RIFF", 4); u32(36 + 48000 * 4); out.write("WAVEfmt ", 8); u32(16); u16(1); u16(2);
            u32(48000); u32(192000); u16(4); u16(16); out.write("data",4); u32(192000);
            // Keep the exact one-second, stereo 48 kHz fixture, but write it as a
            // block. Per-sample iostream calls made the fixture itself dominate a
            // 5,000-track run rather than the library paths being measured.
            std::string pcm(48000 * 4, '\0');
            for (int i = 0; i < 48000; ++i) {
                const auto sample = static_cast<std::int16_t>(i % 100 < 50 ? n + 1 : -(n + 1));
                const auto position = static_cast<std::size_t>(i) * 4;
                pcm[position] = static_cast<char>(sample & 0xff);
                pcm[position + 1] = static_cast<char>((sample >> 8) & 0xff);
            }
            out.write(pcm.data(), static_cast<std::streamsize>(pcm.size()));
            out.close(); files.push_back({util::path_to_utf8(path), ""});
            if ((n + 1) % 500 == 0) std::cerr << "Prepared " << n + 1 << " fixtures\n";
        }
        nlohmann::json results = {{"fixture", "unique 1-second PCM WAV; stereo 48kHz s16; 192044 bytes per track"},
            {"build", "Release Windows MSVC"}, {"runs", nlohmann::json::array()}};
        const auto memory_bytes = [] {
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = sizeof(counters);
            return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) ? static_cast<std::uint64_t>(counters.WorkingSetSize) : 0ull;
#else
            rusage usage{}; getrusage(RUSAGE_SELF, &usage); return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
        };
        for (int count : {100, 500, 1000, 5000}) {
            const auto store = root / std::to_string(count);
            library::JsonTrackRepository repository(store / "library" / "catalog.json", store); library::LibraryService library(repository);
            importing::ImportService imports(library, store / "library", [](const auto&) { return true; }, 1024 * 1024, store / "assets");
            const auto memory_baseline = memory_bytes(); std::atomic<bool> sampling{true}; std::atomic<std::uint64_t> peak_memory{memory_baseline}, peak_journal{0};
            std::thread sampler([&] { while (sampling.load()) { peak_memory.store(std::max(peak_memory.load(), memory_bytes())); const auto journal = store / "library" / "catalog.journal.jsonl"; if (fs::exists(journal)) peak_journal.store(std::max<std::uint64_t>(peak_journal.load(), fs::file_size(journal))); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } });
            const auto start = Clock::now();
            std::vector<importing::ImportBatch> finished_batches;
            for (int offset = 0; offset < count; offset += static_cast<int>(importing::ImportService::max_files)) {
                const auto end = std::min(count, offset + static_cast<int>(importing::ImportService::max_files));
                const auto submitted = imports.submit({files.begin() + offset, files.begin() + end}, "server_path", "benchmark-" + std::to_string(count) + "-" + std::to_string(offset));
                while (true) {
                    auto finished = *imports.get(submitted.id);
                    if (finished.status == "completed" || finished.status == "failed") { finished_batches.push_back(std::move(finished)); break; }
                    if (Clock::now() - start > std::chrono::minutes(10)) throw std::runtime_error("benchmark timeout");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            const auto compact_start = Clock::now(); repository.checkpoint();
            auto compaction_ms = std::chrono::duration<double, std::milli>(Clock::now() - compact_start).count();
            sampling.store(false); sampler.join();
            nlohmann::json run = {{"tracks", count}, {"hash_ms", 0.0}, {"copy_ms", 0.0}, {"metadata_ms", 0.0}, {"persistence_ms", 0.0},
                {"total_ms", std::chrono::duration<double, std::milli>(Clock::now() - start).count()},
                {"catalog_bytes", fs::file_size(store / "library" / "catalog.json")}, {"journal_peak_bytes", peak_journal.load()},
                {"peak_memory_bytes", peak_memory.load()}, {"peak_memory_delta_bytes", peak_memory.load() - memory_baseline},
                {"compaction_ms", compaction_ms}, {"failed_files", 0}, {"journal_persistence_ms",0.0}};
            for (const auto& finished : finished_batches) {
                run["journal_persistence_ms"] = run["journal_persistence_ms"].get<double>() + finished.journal_ms;
                compaction_ms += finished.compaction_ms;
                for (const auto& result : finished.files) {
                    for (const char* stage : {"hash_ms", "copy_ms", "metadata_ms", "persistence_ms"}) run[stage] = run[stage].get<double>() + result.timings[stage].get<double>();
                    if (result.status != "imported") run["failed_files"] = run["failed_files"].get<int>() + 1;
                }
            }
            run["compaction_ms"] = compaction_ms;
            run["catalog_persistence_ms"] = run["persistence_ms"];
            run["persistence_ms"] = run["persistence_ms"].get<double>() + run["journal_persistence_ms"].get<double>() + compaction_ms;
            run["other_ms"] = run["total_ms"].get<double>() - run["hash_ms"].get<double>() - run["copy_ms"].get<double>() - run["metadata_ms"].get<double>() - run["persistence_ms"].get<double>();
            results["runs"].push_back(run); std::cerr << "Completed " << count << " tracks\n";
        }
        std::cout << results.dump(2) << '\n';
        if (argc > 1) { std::ofstream out(util::utf8_to_path(argv[1])); out << results.dump(2) << '\n'; if (!out) throw std::runtime_error("cannot save benchmark results"); }
        fs::remove_all(root); // Unique fixture created only under the benchmark cwd.
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; fs::remove_all(root); return 1; }
}
