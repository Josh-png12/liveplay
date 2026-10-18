#pragma once
// ============================================================================
// Musical analysis (LivePlay+ Phase 2): BPM, beats, beat grid, key, energy and
// structure. Everything here is data + background orchestration. Nothing in this
// header is touched by the audio callback, and no concrete MIR library leaks
// through it: providers (today an external librosa worker; tomorrow Essentia, a
// native analyser or a cloud service) only have to return a MusicalAnalysis JSON
// document that survives validate_musical().
// ============================================================================
#include "liveplay/analysis/analysis.hpp"
#include "liveplay/analysis/worker_adapter.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace liveplay::analysis {

inline constexpr int musical_schema_version = 1;
inline constexpr const char* musical_provider_id = "librosa-mir";

// ---- key / Camelot ---------------------------------------------------------
// Accepts sharps, flats and unicode accidentals ("Db", "C#", "A♭", "f#"); returns
// the canonical sharp spelling ("C#") or nullopt.
std::optional<std::string> normalize_key_name(const std::string& name);
// "F","minor" -> "4A"; "C","major" -> "8B". nullopt for unknown key/scale.
std::optional<std::string> camelot_for(const std::string& key, const std::string& scale);
// Continuous energy 0..1 -> UI level 1..5 (0 / unknown -> 0).
int energy_level(double energy01);
// "high" >= 0.75, "medium" >= 0.5, otherwise "low".
std::string confidence_label(double confidence);

// ---- document --------------------------------------------------------------
// Validates and normalises a provider's musical-analysis document: finite values,
// ranges, strictly increasing beats, ordered non-overlapping sections, Camelot
// recomputed from key/scale (the provider's own value is never trusted), size
// bounds. Throws std::invalid_argument. Unknown section types become "unknown".
json validate_musical(const json& document);
// Compact view for tables: no beat/section arrays, no curves.
json musical_summary(const json& document);

// Persistent store: analysis/musical/<track_id>.json plus a small derived index
// (index.json) that backs list views without opening every document. The index
// is regenerable and rebuilt when it disagrees with the directory.
class MusicalAnalysisStore {
public:
    explicit MusicalAnalysisStore(std::filesystem::path root);
    std::optional<json> find(const std::string& track_id) const;
    void save(const std::string& track_id, json document);
    void remove(const std::string& track_id);
    // Keeps the data but flags it stale (the source asset no longer matches).
    void mark_stale(const std::string& track_id);
    // absent | ready | stale, relative to the library's current content hash.
    std::string state(const std::string& track_id, const std::string& current_hash) const;
    json summaries() const;
private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, json> index_;
    void load_index();
    void rebuild_index();
    void write_index() const;
};

// ---- provider --------------------------------------------------------------
// A MusicalAnalysisProvider is an AnalysisProvider whose result carries a
// `musical` document. Capability names are the public API names.
class MusicalAnalysisProvider : public AnalysisProvider {
public:
    // bpm, beats, beatgrid, downbeats, keyDetection, camelot, energyAnalysis, structureAnalysis
    virtual json musical_capabilities() const = 0;
    virtual std::string provider_version() const { return {}; }
};

// Adapter over the JSON-Lines external worker (tools/mir/liveplay_mir_worker.py).
class ExternalMusicalProvider final : public MusicalAnalysisProvider {
public:
    // Throws when the worker cannot start or reports it is unhealthy (missing Python/librosa).
    ExternalMusicalProvider(std::filesystem::path executable, ExternalWorkerOptions options,
                            std::chrono::milliseconds timeout = std::chrono::seconds(240),
                            std::string provider_id = musical_provider_id);
    std::string id() const override { return provider_id_; }
    json capabilities() const override;
    json musical_capabilities() const override { return capabilities_; }
    json execute(const AnalysisJob& job) override;
    void cancel_current() noexcept override { worker_.cancel(); }
    void release() noexcept override { worker_.release(); }
    std::string provider_version() const override { return provider_version_; }
private:
    std::string provider_id_;
    ExternalAnalysisProvider worker_;
    json capabilities_;
    std::string provider_version_;
};

// Names every capability false; used when no provider is installed.
json unavailable_musical_capabilities();

} // namespace liveplay::analysis
