#pragma once
// ============================================================================
// Real-music validation runner (Phase 2.5). Analyses every audio file of an
// authorised local folder with the musical provider and lets the operator record
// what the right answer was (expected BPM / key / Smart Cue rating), then computes
// accuracy figures. Nothing is imported into the library, nothing is copied, and
// no metric is produced without operator-supplied expectations.
// ============================================================================
#include "liveplay/analysis/musical.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace liveplay::analysis {

struct ParsedKey { std::string key; std::string scale; };
// "F minor", "Fm", "F#m", "Db major", "C", "8A" (Camelot) ... -> sharp-spelled key + scale.
std::optional<ParsedKey> parse_key_label(const std::string& label);
// Same wheel position, an adjacent number on the same ring, or the relative major/minor (same number).
bool camelot_compatible(const std::string& a, const std::string& b);

// Accuracy over the entries that have operator expectations. A figure whose
// denominator is zero is null: never a made-up number.
json validation_metrics(const json& entries, double bpm_tolerance = 1.0);

class MusicValidationRunner {
public:
    using ProviderFactory = std::function<std::shared_ptr<MusicalAnalysisProvider>()>;
    static constexpr std::size_t max_files = 2000;
    MusicValidationRunner(std::filesystem::path root, ProviderFactory factory, std::function<bool()> low_impact = {});
    ~MusicValidationRunner();
    // The folder must already have been authorised by the caller (fs roots).
    json start(const std::filesystem::path& folder, bool recursive);
    json get(const std::string& id) const;                 // throws std::out_of_range
    json list() const;                                     // run headers, newest first
    bool cancel(const std::string& id);
    json update_entry(const std::string& id, std::size_t index, const json& fields);
    std::string export_json(const std::string& id) const;
    std::string export_csv(const std::string& id) const;
private:
    std::filesystem::path root_;
    ProviderFactory factory_;
    std::function<bool()> low_impact_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, json> runs_;
    std::deque<std::string> queue_;
    std::string active_;
    std::atomic<bool> cancel_active_{false};
    std::shared_ptr<MusicalAnalysisProvider> provider_;
    bool stopping_ = false;
    std::thread worker_;
    void run();
    void process(const std::string& id);
    void persist_locked(const std::string& id) const;
    json with_metrics_locked(const std::string& id) const;
};

} // namespace liveplay::analysis
