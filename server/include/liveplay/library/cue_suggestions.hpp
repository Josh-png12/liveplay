#pragma once
// ============================================================================
// Cue suggestions (Smart Cue V1). A CueSuggestionProvider turns whatever is known
// about a track (musical analysis, waveform envelope) into *proposed* IN/OUT
// ranges for generic intents. Suggestions are data only: nothing here creates or
// changes a cue, and the UI always offers Preview / Apply / Reject. Intents are
// event-agnostic ("strong start", "steady background"...) and user code can
// register its own through RuleBasedCueSuggestionProvider::register_intent().
// ============================================================================
#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace liveplay::library {

struct CueSuggestionIntent {
    std::string id;           // stable, kebab-case: "strong-start"
    std::string label;        // default English label; the UI localises by id
    std::string description;
};

struct CueSuggestionInput {
    std::int64_t duration_ms = 0;
    nlohmann::json musical;               // validated MusicalAnalysis, or null
    std::vector<float> waveform_rms;      // optional RMS envelope spanning duration_ms
};

struct CueSuggestion {
    std::string intent, label, reason;
    std::int64_t start_ms = 0, end_ms = 0, fade_in_ms = 0, fade_out_ms = 0;
    double confidence = 0.0;              // 0..1; reflects how much evidence agreed
    std::vector<std::string> basis;       // inputs actually used: energy, sections, beats, downbeats, bpm, waveform
    nlohmann::json to_json() const;
};

struct CueSuggestionSet {
    std::vector<CueSuggestion> suggestions;
    nlohmann::json unavailable = nlohmann::json::array();  // [{intent, reason}] for intents that found no evidence
    nlohmann::json inputs = nlohmann::json::object();      // which inputs were present
    nlohmann::json to_json() const;
};

class CueSuggestionProvider {
public:
    virtual ~CueSuggestionProvider() = default;
    virtual std::string id() const = 0;
    virtual std::vector<CueSuggestionIntent> intents() const = 0;
    // Empty `intent_ids` = every intent. Unknown intent IDs are reported as unavailable.
    virtual CueSuggestionSet suggest(const CueSuggestionInput& input, const std::vector<std::string>& intent_ids = {}) const = 0;
};

// Evidence prepared once per request and shared by the intent handlers.
struct CueSuggestionContext {
    std::int64_t duration_ms = 0;
    double hop_ms = 0;                    // energy-curve resolution
    std::vector<double> energy;           // 0..1 per hop (musical curve, else normalised waveform RMS)
    bool energy_from_musical = false, energy_from_waveform = false;
    std::vector<double> beats_ms, downbeats_ms;
    double bpm = 0;
    struct Section { std::string type; double start_ms, end_ms, confidence, energy; };
    std::vector<Section> sections;
    // Nearest musical grid point (downbeat if known, else beat); the input is returned unchanged without a grid.
    std::int64_t snap(double ms, int direction = 0) const;
    double beat_interval_ms() const;
    double mean_energy(double from_ms, double to_ms) const;
};

class RuleBasedCueSuggestionProvider final : public CueSuggestionProvider {
public:
    using Handler = std::function<std::optional<CueSuggestion>(const CueSuggestionContext&)>;
    RuleBasedCueSuggestionProvider();
    std::string id() const override { return "rule-based-cues"; }
    std::vector<CueSuggestionIntent> intents() const override;
    CueSuggestionSet suggest(const CueSuggestionInput& input, const std::vector<std::string>& intent_ids = {}) const override;
    // Custom intents plug in without touching the built-ins.
    void register_intent(CueSuggestionIntent intent, Handler handler);
private:
    std::vector<CueSuggestionIntent> order_;
    std::map<std::string, Handler> handlers_;
};

} // namespace liveplay::library
