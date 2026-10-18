#include "liveplay/library/cue_suggestions.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace liveplay::library {
using json = nlohmann::json;
namespace {
constexpr double kMinConfidenceToReport = 0.15;
std::string clock(double ms) {
    const auto total = static_cast<long long>(ms / 1000.0);
    char buffer[24]; std::snprintf(buffer, sizeof buffer, "%lld:%02lld", total / 60, total % 60);
    return buffer;
}
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * (v.size() - 1))];
}
double stddev(const std::vector<double>& v, std::size_t a, std::size_t b) {
    if (b <= a + 1) return 0.0;
    double mean = 0; for (auto i = a; i < b; ++i) mean += v[i]; mean /= (b - a);
    double sum = 0; for (auto i = a; i < b; ++i) sum += (v[i] - mean) * (v[i] - mean);
    return std::sqrt(sum / (b - a));
}
std::vector<std::string> basis_of(const CueSuggestionContext& c, bool sections, bool grid_used) {
    std::vector<std::string> basis;
    if (c.energy_from_musical) basis.push_back("energy");
    if (c.energy_from_waveform) basis.push_back("waveform");
    if (sections) basis.push_back("sections");
    if (grid_used) { basis.push_back(c.downbeats_ms.empty() ? "beats" : "downbeats"); if (c.bpm > 0) basis.push_back("bpm"); }
    return basis;
}
// Shared finishing step: grid-snap, clamp into the track, enforce a minimum length.
std::optional<CueSuggestion> finish(const CueSuggestionContext& c, CueSuggestion s, double start, double end, double min_len_ms) {
    s.start_ms = std::clamp<std::int64_t>(c.snap(start, -1), 0, c.duration_ms);
    s.end_ms = std::clamp<std::int64_t>(c.snap(end, 0), 0, c.duration_ms);
    if (static_cast<double>(s.end_ms - s.start_ms) < min_len_ms) return std::nullopt;
    if (s.confidence < kMinConfidenceToReport) return std::nullopt;
    s.confidence = std::round(clamp01(s.confidence) * 1000.0) / 1000.0;
    return s;
}
const CueSuggestionContext::Section* find_section(const CueSuggestionContext& c, std::initializer_list<const char*> types, double min_confidence) {
    const CueSuggestionContext::Section* best = nullptr;
    for (const auto& s : c.sections) {
        if (s.confidence < min_confidence) continue;
        if (std::none_of(types.begin(), types.end(), [&](const char* t) { return s.type == t; })) continue;
        if (!best || s.confidence * (0.5 + s.energy) > best->confidence * (0.5 + best->energy)) best = &s;
    }
    return best;
}

// A sustained high-energy region (a section when one is known, otherwise from the curve).
std::optional<CueSuggestion> strong_start(const CueSuggestionContext& c) {
    CueSuggestion s; s.intent = "strong-start"; s.label = "Strong Start";
    double region_start = -1, region_end = -1, evidence = 0; bool used_section = false;
    if (const auto* drop = find_section(c, {"drop", "chorus"}, 0.35)) {
        region_start = drop->start_ms; region_end = drop->end_ms; evidence = 0.55 + 0.35 * drop->confidence; used_section = true;
        s.reason = "A " + drop->type + " begins at " + clock(drop->start_ms);
    } else if (!c.energy.empty()) {
        const double high = percentile(c.energy, 0.9), low = percentile(c.energy, 0.25);
        if (high - low < 0.12) return std::nullopt;  // flat energy: there is no "strong" moment to start on
        const double threshold = low + 0.7 * (high - low);
        const auto hold = static_cast<std::size_t>(std::max(1.0, 8000.0 / c.hop_ms));
        for (std::size_t i = 0; i + hold <= c.energy.size(); ++i) {
            bool ok = true; for (std::size_t j = i; j < i + hold; ++j) if (c.energy[j] < threshold * 0.95) { ok = false; break; }
            if (!ok) continue;
            region_start = i * c.hop_ms;
            std::size_t k = i; while (k < c.energy.size() && c.energy[k] >= low + 0.5 * (high - low)) ++k;
            region_end = k * c.hop_ms;
            evidence = 0.25 + 0.5 * clamp01((high - low) / 0.5);
            s.reason = "Energy rises to a sustained high level at " + clock(region_start);
            break;
        }
    }
    if (region_start < 0) return std::nullopt;
    const double lead = c.bpm > 0 ? 4.0 * c.beat_interval_ms() : 2000.0;  // one bar of lead-in
    const double start = std::max(0.0, region_start - lead);
    const double end = std::min(region_end, start + 90000.0);
    s.fade_in_ms = c.beats_ms.empty() ? 150 : static_cast<std::int64_t>(std::min(300.0, c.beat_interval_ms() / 2));
    s.fade_out_ms = 1500;
    const bool grid = !c.beats_ms.empty();
    s.confidence = evidence + (grid ? 0.1 : 0.0);
    s.basis = basis_of(c, used_section, grid);
    s.reason += grid ? "; the cue starts one bar earlier on the beat grid." : "; the cue starts shortly before it.";
    return finish(c, std::move(s), start, end, 8000.0);
}

std::optional<CueSuggestion> section_start(const CueSuggestionContext& c) {
    const auto* section = find_section(c, {"verse", "chorus", "build", "drop", "break", "bridge"}, 0.35);
    if (!section) return std::nullopt;
    CueSuggestion s; s.intent = "section-start"; s.label = "Section Start";
    s.fade_in_ms = 100; s.fade_out_ms = 1000;
    s.confidence = 0.4 + 0.5 * section->confidence + (c.beats_ms.empty() ? 0.0 : 0.1);
    s.basis = basis_of(c, true, !c.beats_ms.empty());
    s.reason = "Clean start of the " + section->type + " at " + clock(section->start_ms) + ", aligned to the beat grid when known.";
    return finish(c, std::move(s), section->start_ms, section->end_ms, 4000.0);
}

std::optional<CueSuggestion> high_energy(const CueSuggestionContext& c) {
    if (c.energy.size() < 6) return std::nullopt;
    const double total = c.energy.size() * c.hop_ms;
    const auto window = static_cast<std::size_t>(std::clamp(30000.0, 10000.0, total * 0.5) / c.hop_ms);
    if (window == 0 || window >= c.energy.size()) return std::nullopt;
    double best = -1; std::size_t at = 0, i = 0;
    double sum = std::accumulate(c.energy.begin(), c.energy.begin() + window, 0.0);
    for (; i + window <= c.energy.size(); ++i) {
        if (i) sum += c.energy[i + window - 1] - c.energy[i - 1];
        if (sum > best) { best = sum; at = i; }
    }
    const double window_mean = best / window;
    const double overall = std::accumulate(c.energy.begin(), c.energy.end(), 0.0) / c.energy.size();
    if (window_mean - overall < 0.04) return std::nullopt;  // nowhere is distinctly more energetic
    CueSuggestion s; s.intent = "high-energy"; s.label = "High Energy";
    s.fade_in_ms = 200; s.fade_out_ms = 2000;
    s.confidence = 0.3 + 0.7 * clamp01((window_mean - overall) / 0.25);
    s.basis = basis_of(c, false, !c.beats_ms.empty());
    s.reason = "The most energetic stretch of the track (" + clock(at * c.hop_ms) + "), well above its average level.";
    return finish(c, std::move(s), at * c.hop_ms, (at + window) * c.hop_ms, 8000.0);
}

std::optional<CueSuggestion> steady_background(const CueSuggestionContext& c) {
    if (c.energy.size() < 8) return std::nullopt;
    const double total = c.energy.size() * c.hop_ms;
    double lo_ms = 0, hi_ms = total;
    bool used_sections = false;
    for (const auto& sec : c.sections) {
        if (sec.confidence < 0.35) continue;
        if (sec.type == "intro") { lo_ms = std::max(lo_ms, sec.end_ms); used_sections = true; }
        if (sec.type == "outro") { hi_ms = std::min(hi_ms, sec.start_ms); used_sections = true; }
    }
    const auto window = static_cast<std::size_t>(std::clamp(45000.0, 10000.0, total * 0.8) / c.hop_ms);
    const auto first = static_cast<std::size_t>(lo_ms / c.hop_ms), last = static_cast<std::size_t>(hi_ms / c.hop_ms);
    if (window < 4 || last <= first + window) return std::nullopt;
    double best = 1e9, best_mean = 0; std::size_t at = first;
    for (std::size_t i = first; i + window <= last; ++i) {
        const double sd = stddev(c.energy, i, i + window);
        const double mean = std::accumulate(c.energy.begin() + i, c.energy.begin() + i + window, 0.0) / window;
        if (mean < 0.1 || mean > 0.8) continue;  // silence and peak-level passages are not "background"
        if (sd < best) { best = sd; at = i; best_mean = mean; }
    }
    if (best > 0.12) return std::nullopt;
    CueSuggestion s; s.intent = "steady-background"; s.label = "Steady Background";
    s.fade_in_ms = 1500; s.fade_out_ms = 2500;
    s.confidence = 0.35 + 0.55 * clamp01(1.0 - best / 0.12) + (used_sections ? 0.1 : 0.0);
    s.basis = basis_of(c, used_sections, !c.beats_ms.empty());
    s.reason = "An even-level passage (" + clock(at * c.hop_ms) + " onward, mean energy " + std::to_string(static_cast<int>(best_mean * 100)) + "%) that stays out of the way of speech.";
    return finish(c, std::move(s), at * c.hop_ms, (at + window) * c.hop_ms, 10000.0);
}

std::optional<CueSuggestion> clean_outro(const CueSuggestionContext& c) {
    if (c.duration_ms <= 0) return std::nullopt;
    double end = static_cast<double>(c.duration_ms);
    if (!c.energy.empty()) {   // trim trailing near-silence
        std::size_t last = c.energy.size();
        while (last > 0 && c.energy[last - 1] < 0.03) --last;
        if (last == 0) return std::nullopt;
        end = std::min(end, static_cast<double>(last) * c.hop_ms);
    }
    CueSuggestion s; s.intent = "clean-outro"; s.label = "Clean Outro";
    s.fade_in_ms = 0;
    const auto* outro = find_section(c, {"outro"}, 0.35);
    double start;
    if (outro) {
        start = outro->start_ms; s.confidence = 0.45 + 0.45 * outro->confidence + (c.beats_ms.empty() ? 0.0 : 0.1);
        s.reason = "The outro begins at " + clock(outro->start_ms) + "; playing it from here gives a natural ending.";
    } else {
        const double span = std::min(20000.0, end * 0.3);
        start = end - span;
        const double head = c.mean_energy(0, start), tail = c.mean_energy(start, end);
        if (c.energy.empty() || tail >= head * 0.85) return std::nullopt;  // no evidence that the ending is calmer
        s.confidence = 0.25 + 0.4 * clamp01((head - tail) / 0.3);
        s.reason = "The last " + std::to_string(static_cast<int>(span / 1000)) + " s are clearly quieter than the rest of the track.";
    }
    s.fade_out_ms = static_cast<std::int64_t>(std::min(3000.0, (end - start) / 2));
    s.basis = basis_of(c, outro != nullptr, !c.beats_ms.empty());
    return finish(c, std::move(s), start, end, 4000.0);
}

CueSuggestionContext make_context(const CueSuggestionInput& in) {
    CueSuggestionContext c; c.duration_ms = in.duration_ms;
    const auto& m = in.musical;
    if (m.is_object()) {
        if (m.contains("energyCurve") && m["energyCurve"].is_object()) {
            c.hop_ms = m["energyCurve"].value("hopMs", 1000.0);
            for (const auto& v : m["energyCurve"].value("values", json::array())) c.energy.push_back(v.get<double>());
            c.energy_from_musical = !c.energy.empty();
        }
        if (m.contains("beatsMs")) for (const auto& b : m["beatsMs"]) c.beats_ms.push_back(b.get<double>());
        if (m.contains("downbeatsMs")) for (const auto& b : m["downbeatsMs"]) c.downbeats_ms.push_back(b.get<double>());
        if (m.contains("bpm") && m["bpm"].is_number() && m.value("bpmConfidence", 0.0) >= 0.3) c.bpm = m["bpm"].get<double>();
        if (m.contains("sections")) for (const auto& s : m["sections"])
            c.sections.push_back({s.value("type", std::string("unknown")), s.value("startMs", 0.0), s.value("endMs", 0.0),
                                  s.value("confidence", 0.0), s.contains("energy") && s["energy"].is_number() ? s["energy"].get<double>() : 0.0});
        // Beats of an unreliable tempo estimate would only mislead the snapping.
        if (m.value("bpmConfidence", 0.0) < 0.3) { c.beats_ms.clear(); c.downbeats_ms.clear(); }
    }
    if (c.energy.empty() && in.waveform_rms.size() >= 8 && in.duration_ms > 0) {
        // Resample the display envelope to ~1 s hops and normalise by its 95th percentile.
        const double bucket_ms = static_cast<double>(in.duration_ms) / in.waveform_rms.size();
        c.hop_ms = std::max(bucket_ms, 1000.0);
        const auto per = std::max<std::size_t>(1, static_cast<std::size_t>(c.hop_ms / bucket_ms));
        for (std::size_t i = 0; i < in.waveform_rms.size(); i += per) {
            double sum = 0; std::size_t n = 0;
            for (std::size_t j = i; j < std::min(in.waveform_rms.size(), i + per); ++j, ++n) sum += in.waveform_rms[j];
            c.energy.push_back(n ? sum / n : 0.0);
        }
        c.hop_ms = bucket_ms * per;
        std::vector<double> sorted = c.energy;
        const double reference = percentile(sorted, 0.95);
        if (reference > 1e-4) for (auto& v : c.energy) v = clamp01(v / reference);
        else c.energy.clear();
        c.energy_from_waveform = !c.energy.empty();
    }
    if (c.hop_ms <= 0) c.hop_ms = 1000.0;
    return c;
}
} // namespace

json CueSuggestion::to_json() const {
    return {{"intent", intent}, {"label", label}, {"startMs", start_ms}, {"endMs", end_ms}, {"fadeInMs", fade_in_ms},
            {"fadeOutMs", fade_out_ms}, {"confidence", confidence},
            {"confidenceLabel", confidence >= 0.75 ? "high" : confidence >= 0.5 ? "medium" : "low"},
            {"reason", reason}, {"basis", basis}};
}
json CueSuggestionSet::to_json() const {
    json list = json::array(); for (const auto& s : suggestions) list.push_back(s.to_json());
    return {{"suggestions", list}, {"unavailable", unavailable}, {"inputs", inputs}};
}

std::int64_t CueSuggestionContext::snap(double ms, int direction) const {
    const auto& grid = downbeats_ms.size() >= 4 ? downbeats_ms : beats_ms;
    if (grid.empty()) return static_cast<std::int64_t>(std::llround(ms));
    // direction < 0: last grid point at or before ms; otherwise the nearest.
    auto it = std::lower_bound(grid.begin(), grid.end(), ms);
    if (direction < 0) {
        if (it != grid.end() && std::abs(*it - ms) < 1.0) return static_cast<std::int64_t>(std::llround(*it));
        return static_cast<std::int64_t>(std::llround(it == grid.begin() ? *it : *(it - 1)));
    }
    if (it == grid.begin()) return static_cast<std::int64_t>(std::llround(*it));
    if (it == grid.end()) return static_cast<std::int64_t>(std::llround(ms));  // beyond the last beat: keep the section edge
    return static_cast<std::int64_t>(std::llround(std::abs(*it - ms) < std::abs(*(it - 1) - ms) ? *it : *(it - 1)));
}
double CueSuggestionContext::beat_interval_ms() const { return bpm > 0 ? 60000.0 / bpm : 500.0; }
double CueSuggestionContext::mean_energy(double from_ms, double to_ms) const {
    if (energy.empty() || to_ms <= from_ms) return 0.0;
    const auto a = static_cast<std::size_t>(std::max(0.0, from_ms) / hop_ms);
    const auto b = std::min(energy.size(), std::max(a + 1, static_cast<std::size_t>(to_ms / hop_ms)));
    if (a >= b) return 0.0;
    return std::accumulate(energy.begin() + a, energy.begin() + b, 0.0) / (b - a);
}

RuleBasedCueSuggestionProvider::RuleBasedCueSuggestionProvider() {
    register_intent({"strong-start", "Strong Start", "Begin just before the first sustained high-energy moment."}, strong_start);
    register_intent({"section-start", "Section Start", "Start cleanly on the most clearly detected section."}, section_start);
    register_intent({"high-energy", "High Energy", "The most energetic stretch of the track."}, high_energy);
    register_intent({"steady-background", "Steady Background", "An even, unobtrusive passage that works under speech."}, steady_background);
    register_intent({"clean-outro", "Clean Outro", "Play out the ending without a hard cut."}, clean_outro);
}
void RuleBasedCueSuggestionProvider::register_intent(CueSuggestionIntent intent, Handler handler) {
    if (intent.id.empty() || !handler) throw std::invalid_argument("cue suggestion intent needs an id and a handler");
    if (!handlers_.contains(intent.id)) order_.push_back(intent);
    else for (auto& existing : order_) if (existing.id == intent.id) existing = intent;
    handlers_[intent.id] = std::move(handler);
}
std::vector<CueSuggestionIntent> RuleBasedCueSuggestionProvider::intents() const { return order_; }
CueSuggestionSet RuleBasedCueSuggestionProvider::suggest(const CueSuggestionInput& input, const std::vector<std::string>& ids) const {
    CueSuggestionSet set;
    const auto context = make_context(input);
    set.inputs = {{"musical", input.musical.is_object()}, {"energy", context.energy_from_musical || context.energy_from_waveform},
                  {"waveform", context.energy_from_waveform}, {"sections", !context.sections.empty()},
                  {"beats", !context.beats_ms.empty()}, {"downbeats", !context.downbeats_ms.empty()}};
    std::vector<std::string> wanted = ids;
    if (wanted.empty()) for (const auto& intent : order_) wanted.push_back(intent.id);
    for (const auto& id : wanted) {
        const auto handler = handlers_.find(id);
        if (handler == handlers_.end()) { set.unavailable.push_back({{"intent", id}, {"reason", "unknown intent"}}); continue; }
        std::optional<CueSuggestion> result;
        try { result = handler->second(context); } catch (...) { result = std::nullopt; }
        if (result) {
            for (const auto& intent : order_) if (intent.id == id && result->label.empty()) result->label = intent.label;
            set.suggestions.push_back(std::move(*result));
        } else set.unavailable.push_back({{"intent", id}, {"reason", "not enough reliable evidence for this track"}});
    }
    return set;
}

} // namespace liveplay::library
