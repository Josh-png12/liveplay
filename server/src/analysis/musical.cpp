#include "liveplay/analysis/musical.hpp"
#include "liveplay/util/atomic_json.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>

namespace liveplay::analysis {
namespace fs = std::filesystem;
namespace {
constexpr std::array<const char*, 12> pitch_names{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// Camelot wheel number per pitch class, majors ("B" ring) and minors ("A" ring).
constexpr std::array<int, 12> camelot_major{8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
constexpr std::array<int, 12> camelot_minor{5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10};
constexpr std::size_t max_document_bytes = 4ull * 1024 * 1024;

bool valid_track(const std::string& id) {
    return id.size() == 70 && id.starts_with("track_") && std::all_of(id.begin() + 6, id.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::optional<int> pitch_class(const std::string& key) {
    const auto name = normalize_key_name(key);
    if (!name) return std::nullopt;
    for (int i = 0; i < 12; ++i) if (*name == pitch_names[i]) return i;
    return std::nullopt;
}
std::optional<std::string> normalize_scale(std::string scale) {
    std::transform(scale.begin(), scale.end(), scale.begin(), [](unsigned char c) { return std::tolower(c); });
    if (scale == "major" || scale == "maj") return "major";
    if (scale == "minor" || scale == "min") return "minor";
    return std::nullopt;
}
[[noreturn]] void bad(const std::string& what) { throw std::invalid_argument("invalid musical analysis: " + what); }
// Reads a nullable finite number within [lo, hi].
std::optional<double> number(const json& object, const char* key, double lo, double hi) {
    if (!object.contains(key) || object[key].is_null()) return std::nullopt;
    if (!object[key].is_number()) bad(std::string(key) + " is not a number");
    const double value = object[key].get<double>();
    if (!std::isfinite(value) || value < lo || value > hi) bad(std::string(key) + " out of range");
    return value;
}
json number_or_null(const std::optional<double>& value) { return value ? json(*value) : json(nullptr); }
std::string text(const json& object, const char* key, std::size_t limit, bool required) {
    if (!object.contains(key) || object[key].is_null()) { if (required) bad(std::string(key) + " missing"); return {}; }
    if (!object[key].is_string()) bad(std::string(key) + " is not a string");
    auto value = object[key].get<std::string>();
    if (value.size() > limit || (required && value.empty())) bad(std::string(key) + " has invalid length");
    return value;
}
json times(const json& object, const char* key, std::size_t limit, double max_ms) {
    json out = json::array();
    if (!object.contains(key) || object[key].is_null()) return out;
    if (!object[key].is_array() || object[key].size() > limit) bad(std::string(key) + " is not a bounded array");
    double previous = -1.0;
    for (const auto& item : object[key]) {
        if (!item.is_number()) bad(std::string(key) + " contains a non-number");
        const double value = item.get<double>();
        if (!std::isfinite(value) || value < 0 || value > max_ms) bad(std::string(key) + " value out of range");
        if (value <= previous) bad(std::string(key) + " is not strictly increasing");
        previous = value; out.push_back(value);
    }
    return out;
}
const std::set<std::string> section_types{"intro", "verse", "chorus", "build", "drop", "break", "bridge", "outro", "unknown"};
}

std::optional<std::string> normalize_key_name(const std::string& input) {
    std::string s = input;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return std::nullopt;
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    int semitones;
    switch (letter) {
        case 'C': semitones = 0; break; case 'D': semitones = 2; break; case 'E': semitones = 4; break;
        case 'F': semitones = 5; break; case 'G': semitones = 7; break; case 'A': semitones = 9; break;
        case 'B': semitones = 11; break; default: return std::nullopt;
    }
    const auto accidental = s.substr(1);
    if (accidental == "#" || accidental == "\xE2\x99\xAF") ++semitones;
    else if (accidental == "b" || accidental == "\xE2\x99\xAD") --semitones;
    else if (!accidental.empty()) return std::nullopt;
    return std::string(pitch_names[((semitones % 12) + 12) % 12]);
}

std::optional<std::string> camelot_for(const std::string& key, const std::string& scale) {
    const auto pitch = pitch_class(key);
    const auto mode = normalize_scale(scale);
    if (!pitch || !mode) return std::nullopt;
    return std::to_string(*mode == "major" ? camelot_major[*pitch] : camelot_minor[*pitch]) + (*mode == "major" ? "B" : "A");
}

int energy_level(double energy) {
    if (!std::isfinite(energy) || energy <= 0.0) return 0;
    return std::clamp(1 + static_cast<int>(std::floor(std::min(energy, 0.9999) * 5.0)), 1, 5);
}

std::string confidence_label(double confidence) {
    return confidence >= 0.75 ? "high" : confidence >= 0.5 ? "medium" : "low";
}

json unavailable_musical_capabilities() {
    return {{"bpm", false}, {"beats", false}, {"beatgrid", false}, {"downbeats", false}, {"keyDetection", false},
            {"camelot", false}, {"energyAnalysis", false}, {"structureAnalysis", false}};
}

json validate_musical(const json& in) {
    if (!in.is_object()) bad("not an object");
    if (in.dump().size() > max_document_bytes) throw std::length_error("musical analysis too large");
    if (in.value("schemaVersion", 0) != musical_schema_version) bad("unsupported schemaVersion");
    json out = json::object();
    out["schemaVersion"] = musical_schema_version;
    out["provider"] = text(in, "provider", 64, true);
    out["providerVersion"] = text(in, "providerVersion", 128, false);
    out["analyzedAt"] = text(in, "analyzedAt", 40, true);
    const auto hash = text(in, "sourceHash", 80, true);
    if (!hash.starts_with("sha256:")) bad("sourceHash must be a sha256: hash");
    out["sourceHash"] = hash;
    const auto duration = number(in, "durationMs", 0, 24.0 * 3600 * 1000);
    out["durationMs"] = number_or_null(duration);
    const double horizon = (duration ? *duration : 24.0 * 3600 * 1000) + 2000.0;
    out["status"] = "completed";
    out["stale"] = false;
    out["corrections"] = json::object();  // reserved for manual BPM/beatgrid/key overrides; never written by providers
    json warnings = json::array();
    if (in.contains("warnings") && in["warnings"].is_array())
        for (const auto& w : in["warnings"]) if (w.is_string() && warnings.size() < 16) warnings.push_back(w.get<std::string>().substr(0, 200));
    out["warnings"] = warnings;

    // ---- rhythm
    const auto bpm = number(in, "bpm", 20, 400);
    const auto bpm_confidence = number(in, "bpmConfidence", 0, 1);
    out["bpm"] = number_or_null(bpm);
    out["bpmConfidence"] = number_or_null(bpm_confidence);
    out["bpmConfidenceLabel"] = bpm_confidence ? json(confidence_label(*bpm_confidence)) : json(nullptr);
    out["bpmAmbiguous"] = in.value("bpmAmbiguous", false) == true;
    json alternatives = json::array();
    if (in.contains("bpmAlternatives") && in["bpmAlternatives"].is_array())
        for (const auto& a : in["bpmAlternatives"]) {
            if (alternatives.size() >= 8) break;
            const auto value = number(a, "bpm", 20, 400);
            if (!value) continue;
            alternatives.push_back({{"bpm", *value}, {"relation", a.value("relation", std::string("other")).substr(0, 24)},
                                    {"support", number_or_null(number(a, "support", 0, 100))}});
        }
    out["bpmAlternatives"] = alternatives;
    out["beatsMs"] = times(in, "beatsMs", 32768, horizon);
    out["downbeatsMs"] = times(in, "downbeatsMs", 16384, horizon);
    out["downbeatsAvailable"] = !out["downbeatsMs"].empty();
    out["beatGrid"] = nullptr;
    if (in.contains("beatGrid") && in["beatGrid"].is_object()) {
        const auto& g = in["beatGrid"];
        const auto first = number(g, "firstBeatMs", 0, horizon);
        const auto interval = number(g, "intervalMs", 1, 60000);
        const auto tempo = number(g, "tempo", 20, 400);
        if (!first || !interval || !tempo) bad("beatGrid is incomplete");
        json segments = json::array();
        double previous_end = -1;
        if (g.contains("segments") && g["segments"].is_array()) {
            if (g["segments"].size() > 512) bad("beatGrid has too many segments");
            for (const auto& seg : g["segments"]) {
                const auto a = number(seg, "startMs", 0, horizon), b = number(seg, "endMs", 0, horizon), sb = number(seg, "bpm", 20, 400);
                if (!a || !b || !sb || *b < *a || *a < previous_end) bad("beatGrid segments are not ordered");
                previous_end = *b;
                segments.push_back({{"startMs", *a}, {"endMs", *b}, {"bpm", *sb}, {"beatCount", seg.value("beatCount", 0)}});
            }
        }
        out["beatGrid"] = {{"firstBeatMs", *first}, {"intervalMs", *interval}, {"tempo", *tempo},
            {"confidence", number_or_null(number(g, "confidence", 0, 1))},
            {"constantTempo", g.value("constantTempo", segments.size() <= 1)}, {"segments", segments}};
    }

    // ---- tonality (Camelot is always derived here)
    out["key"] = nullptr; out["scale"] = nullptr; out["camelot"] = nullptr;
    out["keyConfidence"] = nullptr; out["keyConfidenceLabel"] = nullptr;
    if (in.contains("key") && !in["key"].is_null()) {
        const auto key = in["key"].is_string() ? normalize_key_name(in["key"].get<std::string>()) : std::nullopt;
        const auto scale = in.contains("scale") && in["scale"].is_string() ? normalize_scale(in["scale"].get<std::string>()) : std::nullopt;
        if (!key || !scale) bad("unrecognised key/scale");
        out["key"] = *key; out["scale"] = *scale; out["camelot"] = *camelot_for(*key, *scale);
        const auto conf = number(in, "keyConfidence", 0, 1);
        out["keyConfidence"] = number_or_null(conf);
        out["keyConfidenceLabel"] = conf ? json(confidence_label(*conf)) : json(nullptr);
    }
    if (const auto clarity = number(in, "tonalClarity", 0, 100)) out["tonalClarity"] = *clarity;

    // ---- energy
    const auto energy = number(in, "energy", 0, 1);
    out["energy"] = number_or_null(energy);
    out["energyLevel"] = energy ? json(energy_level(*energy)) : json(nullptr);
    out["energyConfidence"] = number_or_null(number(in, "energyConfidence", 0, 1));
    out["energyCurve"] = nullptr;
    if (in.contains("energyCurve") && in["energyCurve"].is_object()) {
        const auto& c = in["energyCurve"];
        const auto hop = number(c, "hopMs", 100, 600000);
        if (!hop || !c.contains("values") || !c["values"].is_array() || c["values"].size() > 4096) bad("energyCurve is invalid");
        json values = json::array();
        for (const auto& v : c["values"]) {
            if (!v.is_number()) bad("energyCurve contains a non-number");
            const double value = v.get<double>();
            if (!std::isfinite(value) || value < 0 || value > 1) bad("energyCurve value out of range");
            values.push_back(value);
        }
        out["energyCurve"] = {{"hopMs", static_cast<int>(*hop)}, {"values", values}};
    }

    // ---- structure
    json sections = json::array();
    if (in.contains("sections") && in["sections"].is_array()) {
        if (in["sections"].size() > 256) bad("too many sections");
        double previous_end = 0;
        for (const auto& s : in["sections"]) {
            const auto a = number(s, "startMs", 0, horizon), b = number(s, "endMs", 0, horizon);
            const auto conf = number(s, "confidence", 0, 1);
            if (!a || !b || !conf || *b <= *a) bad("section boundaries invalid");
            if (*a < previous_end - 1.0) bad("sections overlap or are unordered");
            previous_end = *b;
            auto type = s.value("type", std::string("unknown"));
            if (!section_types.contains(type)) type = "unknown";
            sections.push_back({{"type", type}, {"startMs", *a}, {"endMs", *b}, {"confidence", *conf},
                                {"energy", number_or_null(number(s, "energy", 0, 1))}});
        }
    }
    out["sections"] = sections;

    // Optional per-stage wall times from the worker (diagnostics only).
    if (in.contains("timingsMs") && in["timingsMs"].is_object()) {
        json timings = json::object();
        for (const auto& [name, value] : in["timingsMs"].items())
            if (value.is_number() && std::isfinite(value.get<double>()) && timings.size() < 16) timings[name.substr(0, 32)] = value.get<double>();
        out["timingsMs"] = timings;
    }
    json capabilities = json::object();
    if (in.contains("capabilities") && in["capabilities"].is_object())
        for (const auto& [name, value] : in["capabilities"].items()) {
            if (value.is_boolean()) capabilities[name] = value;
            else if (name == "sectionTypes" && value.is_array()) {
                json kinds = json::array();
                for (const auto& k : value) if (k.is_string() && kinds.size() < 16) kinds.push_back(k.get<std::string>().substr(0, 24));
                capabilities[name] = kinds;
            }
        }
    out["capabilities"] = capabilities;
    return out;
}

json empty_corrections() {
    return {{"bpm", nullptr}, {"firstBeatMs", nullptr}, {"gridOffsetMs", 0.0}, {"key", nullptr}, {"scale", nullptr}};
}

json validate_corrections(const json& in, const json& detected) {
    if (!in.is_object()) bad("corrections is not an object");
    json out = empty_corrections();
    const auto detected_bpm = number(detected, "bpm", 20, 400);
    const double duration = number(detected, "durationMs", 0, 24.0 * 3600 * 1000).value_or(24.0 * 3600 * 1000);
    if (in.contains("bpm") && !in["bpm"].is_null()) {
        const auto& b = in["bpm"];
        if (!b.is_object() || !b.contains("value") || !b["value"].is_number()) bad("bpm correction needs {mode, value}");
        const auto mode = b.value("mode", std::string());
        const double value = b["value"].get<double>();
        if (!std::isfinite(value)) bad("bpm correction is not finite");
        if (mode == "factor") {
            if (!detected_bpm) bad("a BPM factor needs a detected BPM");
            if (value < 0.25 || value > 4.0) bad("bpm factor out of range");
            if (*detected_bpm * value < 20 || *detected_bpm * value > 400) bad("corrected bpm out of range");
        } else if (mode == "custom") {
            if (value < 20 || value > 400) bad("custom bpm out of range");
        } else bad("bpm correction mode must be factor or custom");
        out["bpm"] = {{"mode", mode}, {"value", value}};
    }
    if (in.contains("firstBeatMs") && !in["firstBeatMs"].is_null()) {
        const auto value = number(in, "firstBeatMs", 0, duration + 2000.0);
        out["firstBeatMs"] = *value;
    }
    if (const auto offset = number(in, "gridOffsetMs", -60000, 60000)) out["gridOffsetMs"] = *offset;
    if (in.contains("key") && !in["key"].is_null()) {
        const auto key = in["key"].is_string() ? normalize_key_name(in["key"].get<std::string>()) : std::nullopt;
        if (!key) bad("unrecognised key correction");
        out["key"] = *key;
    }
    if (in.contains("scale") && !in["scale"].is_null()) {
        const auto scale = in["scale"].is_string() ? normalize_scale(in["scale"].get<std::string>()) : std::nullopt;
        if (!scale) bad("scale correction must be major or minor");
        out["scale"] = *scale;
    }
    // A grid correction needs a tempo to build on: a corrected BPM or a detected one.
    const bool grid_touched = !out["firstBeatMs"].is_null() || out["gridOffsetMs"].get<double>() != 0.0;
    if (grid_touched && out["bpm"].is_null() && !detected_bpm) bad("grid correction needs a BPM (detected or corrected)");
    return out;
}

namespace {
bool has_grid_correction(const json& c) {
    return c.is_object() && (!c.value("bpm", json(nullptr)).is_null() || !c.value("firstBeatMs", json(nullptr)).is_null() || c.value("gridOffsetMs", 0.0) != 0.0);
}
json corrected_grid(const json& d, double bpm, const json& c) {
    const double interval = 60000.0 / bpm;
    double first;
    if (!c.value("firstBeatMs", json(nullptr)).is_null()) first = c["firstBeatMs"].get<double>();
    else if (d.contains("beatGrid") && d["beatGrid"].is_object()) first = d["beatGrid"].value("firstBeatMs", 0.0);
    else if (d.contains("beatsMs") && d["beatsMs"].is_array() && !d["beatsMs"].empty()) first = d["beatsMs"][0].get<double>();
    else first = 0.0;
    first += c.value("gridOffsetMs", 0.0);
    first = std::fmod(first, interval);          // the grid is periodic; keep its first beat inside the first interval
    if (first < 0) first += interval;
    const double end = d.value("durationMs", json(nullptr)).is_number() ? d["durationMs"].get<double>() : first + interval;
    return {{"firstBeatMs", first}, {"intervalMs", interval}, {"tempo", bpm}, {"confidence", 1.0}, {"constantTempo", true},
            {"segments", json::array({{{"startMs", first}, {"endMs", std::max(end, first)}, {"bpm", bpm}}})}};
}
}

json effective_values(const json& d) {
    const json c = d.contains("corrections") && d["corrections"].is_object() ? d["corrections"] : json::object();
    json out = {{"bpmSource", "detected"}, {"gridSource", "detected"}, {"keySource", "detected"}, {"corrected", false}};
    const auto detected_bpm = d.contains("bpm") && d["bpm"].is_number() ? std::optional<double>(d["bpm"].get<double>()) : std::nullopt;
    std::optional<double> bpm = detected_bpm;
    out["bpmConfidence"] = d.value("bpmConfidence", json(nullptr));
    out["bpmAmbiguous"] = d.value("bpmAmbiguous", false);
    if (c.contains("bpm") && c["bpm"].is_object()) {
        const auto& b = c["bpm"];
        if (b.value("mode", std::string()) == "factor" && detected_bpm) { bpm = *detected_bpm * b["value"].get<double>(); out["bpmSource"] = "factor"; }
        else if (b.value("mode", std::string()) == "custom") { bpm = b["value"].get<double>(); out["bpmSource"] = "custom"; }
        if (out["bpmSource"] != "detected") { out["bpmConfidence"] = 1.0; out["bpmAmbiguous"] = false; out["corrected"] = true; }
    }
    out["bpm"] = bpm ? json(*bpm) : json(nullptr);
    out["beatGrid"] = d.value("beatGrid", json(nullptr));
    if (bpm && has_grid_correction(c)) {
        out["beatGrid"] = corrected_grid(d, *bpm, c); out["gridSource"] = "corrected"; out["corrected"] = true;
    }
    json key = d.value("key", json(nullptr)), scale = d.value("scale", json(nullptr));
    if (c.contains("key") && c["key"].is_string()) { key = c["key"]; out["keySource"] = "corrected"; }
    if (c.contains("scale") && c["scale"].is_string()) { scale = c["scale"]; out["keySource"] = "corrected"; }
    if (out["keySource"] == "corrected") out["corrected"] = true;
    out["key"] = key; out["scale"] = scale;
    const auto code = key.is_string() && scale.is_string() ? camelot_for(key.get<std::string>(), scale.get<std::string>()) : std::nullopt;
    out["camelot"] = code ? json(*code) : json(nullptr);
    return out;
}

std::vector<double> effective_beats(const json& d) {
    std::vector<double> beats;
    const auto effective = effective_values(d);
    if (effective["gridSource"] == "corrected" && effective["beatGrid"].is_object()) {
        const auto& g = effective["beatGrid"];
        const double interval = g["intervalMs"].get<double>(), end = d.value("durationMs", json(nullptr)).is_number() ? d["durationMs"].get<double>() : 0.0;
        for (double t = g["firstBeatMs"].get<double>(); t <= end && beats.size() < 32768; t += interval) beats.push_back(t);
    } else if (d.contains("beatsMs") && d["beatsMs"].is_array()) {
        for (const auto& b : d["beatsMs"]) beats.push_back(b.get<double>());
    }
    return beats;
}

json consumer_view(const json& d) {
    json view = d;
    const auto e = effective_values(d);
    view["bpm"] = e["bpm"]; view["bpmConfidence"] = e["bpmConfidence"]; view["bpmAmbiguous"] = e["bpmAmbiguous"];
    view["beatGrid"] = e["beatGrid"]; view["key"] = e["key"]; view["scale"] = e["scale"]; view["camelot"] = e["camelot"];
    const auto beats = effective_beats(d);
    view["beatsMs"] = json(beats);
    return view;
}

json musical_summary(const json& d) {
    json effective = json::object();
    try { effective = effective_values(d); } catch (...) {}
    return {{"effectiveBpm", effective.value("bpm", json(nullptr))}, {"effectiveKey", effective.value("key", json(nullptr))},
        {"effectiveScale", effective.value("scale", json(nullptr))}, {"effectiveCamelot", effective.value("camelot", json(nullptr))},
        {"effectiveBpmConfidenceLabel", effective.value("bpmConfidence", json(nullptr)).is_number() ? json(confidence_label(effective["bpmConfidence"].get<double>())) : json(nullptr)},
        {"corrected", effective.value("corrected", false)},
        {"trackId", d.value("trackId", std::string())}, {"provider", d.value("provider", std::string())},
        {"analyzedAt", d.value("analyzedAt", std::string())}, {"sourceHash", d.value("sourceHash", std::string())},
        {"stale", d.value("stale", false)},
        {"bpm", d.value("bpm", json(nullptr))}, {"bpmConfidence", d.value("bpmConfidence", json(nullptr))},
        {"bpmConfidenceLabel", d.value("bpmConfidenceLabel", json(nullptr))}, {"bpmAmbiguous", d.value("bpmAmbiguous", false)},
        {"key", d.value("key", json(nullptr))}, {"scale", d.value("scale", json(nullptr))},
        {"camelot", d.value("camelot", json(nullptr))}, {"keyConfidence", d.value("keyConfidence", json(nullptr))},
        {"energy", d.value("energy", json(nullptr))}, {"energyLevel", d.value("energyLevel", json(nullptr))},
        {"sectionCount", d.contains("sections") ? d["sections"].size() : 0},
        {"beatCount", d.contains("beatsMs") ? d["beatsMs"].size() : 0},
        {"hasDownbeats", d.value("downbeatsAvailable", false)}};
}

// ---- store -----------------------------------------------------------------
MusicalAnalysisStore::MusicalAnalysisStore(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
    load_index();
}
void MusicalAnalysisStore::load_index() {
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(root_))
        if (entry.is_regular_file() && entry.path().extension() == ".json" && entry.path().filename() != "index.json") ++files;
    try {
        const auto path = root_ / "index.json";
        if (fs::exists(path)) {
            const auto doc = util::read_bounded_json(path, 32ull * 1024 * 1024);
            if (doc.at("schema_version") == 1 && doc.at("tracks").is_object())
                for (const auto& [id, summary] : doc.at("tracks").items()) if (valid_track(id)) index_[id] = summary;
        }
    } catch (...) { index_.clear(); }
    if (index_.size() != files) rebuild_index();
}
void MusicalAnalysisStore::rebuild_index() {
    index_.clear();
    for (const auto& entry : fs::directory_iterator(root_)) {
        const auto name = entry.path().stem().string();
        if (!entry.is_regular_file() || entry.path().extension() != ".json" || !valid_track(name)) continue;
        try {
            auto doc = util::read_bounded_json(entry.path(), max_document_bytes * 2);
            doc["trackId"] = name;
            index_[name] = musical_summary(doc);
        } catch (...) { /* a corrupt document is simply absent; re-analysis regenerates it */ }
    }
    try { write_index(); } catch (...) {}
}
void MusicalAnalysisStore::write_index() const {
    json tracks = json::object();
    for (const auto& [id, summary] : index_) tracks[id] = summary;
    util::atomic_write_json(root_ / "index.json", {{"schema_version", 1}, {"tracks", tracks}});
}
std::optional<json> MusicalAnalysisStore::find(const std::string& id) const {
    if (!valid_track(id)) throw std::invalid_argument("invalid track ID");
    std::lock_guard lock(mutex_);
    const auto path = root_ / (id + ".json");
    if (!fs::exists(path)) return std::nullopt;
    auto doc = util::read_bounded_json(path, max_document_bytes * 2);
    doc["trackId"] = id;
    try { doc["effective"] = effective_values(doc); } catch (...) { doc["effective"] = nullptr; }
    return doc;
}
void MusicalAnalysisStore::save(const std::string& id, json doc) {
    if (!valid_track(id)) throw std::invalid_argument("invalid track ID");
    doc = validate_musical(doc);
    doc["trackId"] = id;
    std::lock_guard lock(mutex_);
    // Re-analysis of the same audio keeps the operator's corrections; changed audio invalidates them.
    if (const auto previous_path = root_ / (id + ".json"); fs::exists(previous_path)) {
        try {
            const auto previous = util::read_bounded_json(previous_path, max_document_bytes * 2);
            if (previous.value("sourceHash", std::string()) == doc.value("sourceHash", std::string()) && previous.contains("corrections") && previous["corrections"].is_object())
                doc["corrections"] = validate_corrections(previous["corrections"], doc);
        } catch (...) { /* unreadable or no longer valid corrections are dropped */ }
    }
    util::atomic_write_json(root_ / (id + ".json"), doc);
    index_[id] = musical_summary(doc);
    write_index();
}
void MusicalAnalysisStore::remove(const std::string& id) {
    if (!valid_track(id)) throw std::invalid_argument("invalid track ID");
    std::lock_guard lock(mutex_);
    std::error_code ignored; fs::remove(root_ / (id + ".json"), ignored);
    if (index_.erase(id)) write_index();
}
void MusicalAnalysisStore::mark_stale(const std::string& id) {
    if (!valid_track(id)) throw std::invalid_argument("invalid track ID");
    std::lock_guard lock(mutex_);
    const auto path = root_ / (id + ".json");
    if (!fs::exists(path)) return;
    auto doc = util::read_bounded_json(path, max_document_bytes * 2);
    if (doc.value("stale", false)) return;
    doc["stale"] = true;
    util::atomic_write_json(path, doc);
    doc["trackId"] = id;
    index_[id] = musical_summary(doc);
    write_index();
}
json MusicalAnalysisStore::set_corrections(const std::string& id, const json& corrections) {
    if (!valid_track(id)) throw std::invalid_argument("invalid track ID");
    std::lock_guard lock(mutex_);
    const auto path = root_ / (id + ".json");
    if (!fs::exists(path)) throw std::invalid_argument("track has no musical analysis to correct");
    auto doc = util::read_bounded_json(path, max_document_bytes * 2);
    doc["corrections"] = validate_corrections(corrections, doc);
    util::atomic_write_json(path, doc);
    doc["trackId"] = id;
    index_[id] = musical_summary(doc);
    write_index();
    doc["effective"] = effective_values(doc);
    return doc;
}
std::string MusicalAnalysisStore::state(const std::string& id, const std::string& current_hash) const {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(id);
    if (it == index_.end()) return "absent";
    if (it->second.value("stale", false)) return "stale";
    if (!current_hash.empty() && it->second.value("sourceHash", std::string()) != current_hash) return "stale";
    return "ready";
}
json MusicalAnalysisStore::summaries() const {
    std::lock_guard lock(mutex_);
    json out = json::object();
    for (const auto& [id, summary] : index_) { out[id] = summary; out[id]["trackId"] = id; }
    return out;
}

// ---- external provider -----------------------------------------------------
ExternalMusicalProvider::ExternalMusicalProvider(fs::path executable, ExternalWorkerOptions options,
                                                 std::chrono::milliseconds timeout, std::string provider_id)
    : provider_id_(provider_id), worker_(std::move(executable), timeout, provider_id, json::object(), std::move(options)) {
    const auto health = worker_.health_info();
    if (health.value("status", "") != "ok") throw std::runtime_error(health.value("error", std::string("musical analysis worker unhealthy")));
    capabilities_ = unavailable_musical_capabilities();
    const auto reported = health.value("capabilities", json::object());
    for (auto& [name, value] : capabilities_.items()) value = reported.value(name, false) == true;
    if (reported.contains("sectionTypes") && reported["sectionTypes"].is_array()) capabilities_["sectionTypes"] = reported["sectionTypes"];
    provider_version_ = health.value("providerVersion", std::string());
}
json ExternalMusicalProvider::capabilities() const {
    // Legacy capability keys map onto the musical ones so older clients keep working.
    return {{"basic", false}, {"loudness", false}, {"rhythm", capabilities_.value("bpm", false)},
            {"tonality", capabilities_.value("keyDetection", false)}, {"structure", capabilities_.value("structureAnalysis", false)},
            {"semantic", false}, {"stems", false}};
}
namespace {
long long now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
ExternalMusicalProvider::~ExternalMusicalProvider() {
    stopping_ = true;
    worker_.cancel();
    if (warm_thread_.joinable()) warm_thread_.join();
}
std::string ExternalMusicalProvider::warmup_state() const { std::lock_guard lock(warm_mutex_); return warm_state_; }
json ExternalMusicalProvider::last_warmup() const { std::lock_guard lock(warm_mutex_); return warm_result_; }
bool ExternalMusicalProvider::warm_up_async(std::function<bool()> blocked, std::chrono::seconds idle_release) {
    {
        std::lock_guard lock(warm_mutex_);
        if (warm_state_ == "warming" || (warm_state_ == "warm" && now_ms() - last_activity_ms_ < 60'000)) return false;
        if (blocked && blocked()) return false;
        warm_state_ = "warming";
    }
    if (warm_thread_.joinable()) warm_thread_.join();   // a previous warm-up has finished (state != warming)
    last_activity_ms_ = now_ms();
    warm_thread_ = std::thread([this, idle_release] {
        json outcome;
        try {
            const auto reply = worker_.call("warmup");
            outcome = reply.value("status", "") == "ok" ? json{{"status", "ok"}, {"warmupMs", reply.value("warmupMs", 0.0)}} : json{{"status", "error"}, {"error", reply.value("error", std::string("warm-up failed"))}};
        } catch (const std::exception& e) { outcome = {{"status", "error"}, {"error", e.what()}}; }
        {
            std::lock_guard lock(warm_mutex_);
            warm_result_ = outcome; warm_state_ = outcome["status"] == "ok" ? "warm" : "failed";
        }
        last_activity_ms_ = now_ms();
        // Keep the warmed worker only while it is useful; analysis jobs refresh last_activity_ms_.
        while (!stopping_ && outcome["status"] == "ok" && now_ms() - last_activity_ms_ < std::chrono::duration_cast<std::chrono::milliseconds>(idle_release).count())
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!stopping_) { worker_.release(); std::lock_guard lock(warm_mutex_); if (warm_state_ == "warm") warm_state_ = "idle"; }
    });
    return true;
}
json ExternalMusicalProvider::execute(const AnalysisJob& job) {
    last_activity_ms_ = now_ms();
    auto result = worker_.execute(job);
    last_activity_ms_ = now_ms();
    if (!result.contains("musical")) throw std::runtime_error("musical worker returned no analysis");
    auto document = validate_musical(result["musical"]);
    if (document.at("sourceHash") != job.content_hash) throw std::runtime_error("musical analysis belongs to a different source hash");
    document["trackId"] = job.track_id;
    result["musical"] = std::move(document);
    return result;
}

} // namespace liveplay::analysis
