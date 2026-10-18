#include "liveplay/analysis/validation_runner.hpp"
#include "liveplay/library/cue_suggestions.hpp"
#include "liveplay/library/maintenance.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>

namespace liveplay::analysis {
namespace fs = std::filesystem;
namespace {
std::string iso_now() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buffer[32]; std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; }
bool audio_extension(const fs::path& p) {
    static const std::set<std::string> known{".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".aif", ".aiff", ".opus", ".wma", ".mp4"};
    return known.contains(lower(util::path_to_utf8(p.extension())));
}
bool valid_run_id(const std::string& id) {
    return id.size() > 4 && id.size() < 64 && id.starts_with("run_") && std::all_of(id.begin() + 4, id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}
std::optional<std::pair<int, char>> parse_camelot(const std::string& code) {
    static const std::regex pattern("^([0-9]{1,2})([ABab])$");
    std::smatch m;
    if (!std::regex_match(code, m, pattern)) return std::nullopt;
    const int n = std::stoi(m[1]);
    if (n < 1 || n > 12) return std::nullopt;
    return std::make_pair(n, static_cast<char>(std::toupper(m[2].str()[0])));
}
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
}
json ratio(int ok, int total) { return total ? json(static_cast<double>(ok) / total) : json(nullptr); }
}

std::optional<ParsedKey> parse_key_label(const std::string& input) {
    std::string s = input;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return std::nullopt;
    if (const auto camelot = parse_camelot(s)) {
        // Inverse of the Camelot wheel: find the key whose code matches.
        for (const char* name : {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"})
            for (const char* scale : {"major", "minor"})
                if (const auto code = camelot_for(name, scale); code && parse_camelot(*code) == camelot) return ParsedKey{name, scale};
        return std::nullopt;
    }
    std::size_t length = 1;   // the note letter
    if (s.size() > 1 && (s[1] == '#' || s[1] == 'b')) length = 2;
    else if (s.size() > 3 && (s.compare(1, 3, "\xE2\x99\xAF") == 0 || s.compare(1, 3, "\xE2\x99\xAD") == 0)) length = 4;
    const auto key = normalize_key_name(s.substr(0, length));
    if (!key) return std::nullopt;
    auto rest = lower(s.substr(length));
    rest.erase(std::remove_if(rest.begin(), rest.end(), [](unsigned char c) { return std::isspace(c); }), rest.end());
    if (rest.empty() || rest == "maj" || rest == "major") return ParsedKey{*key, "major"};
    if (rest == "m" || rest == "min" || rest == "minor") return ParsedKey{*key, "minor"};
    return std::nullopt;
}

bool camelot_compatible(const std::string& a, const std::string& b) {
    const auto x = parse_camelot(a), y = parse_camelot(b);
    if (!x || !y) return false;
    if (x->second == y->second) { const int d = std::abs(x->first - y->first); return d == 0 || d == 1 || d == 11; }
    return x->first == y->first;   // relative major / minor
}

json validation_metrics(const json& entries, double tolerance) {
    int analysed = 0, failed = 0, bpm_total = 0, bpm_exact = 0, bpm_octave = 0, key_total = 0, key_exact = 0, key_compatible = 0;
    int good = 0, acceptable = 0, bad = 0;
    std::vector<double> errors, octave_errors, times, warm_times;
    json failures = json::array();
    bool first_done = true;
    for (const auto& e : entries) {
        const auto status = e.value("status", std::string());
        if (status == "failed") { ++failed; failures.push_back({{"filename", e.value("filename", std::string())}, {"error", e.value("error", std::string())}}); }
        if (status != "done") continue;
        ++analysed;
        if (e.contains("analysisDurationMs") && e["analysisDurationMs"].is_number()) {
            const double ms = e["analysisDurationMs"].get<double>(); times.push_back(ms);
            if (!first_done) warm_times.push_back(ms);
            first_done = false;
        }
        const auto rating = e.value("smartCueRating", json(nullptr));
        if (rating == "Good") ++good; else if (rating == "Acceptable") ++acceptable; else if (rating == "Bad") ++bad;
        if (e.contains("expectedBpm") && e["expectedBpm"].is_number() && e["expectedBpm"].get<double>() > 0) {
            ++bpm_total;
            const double expected = e["expectedBpm"].get<double>();
            if (e.contains("detectedBpm") && e["detectedBpm"].is_number()) {
                const double detected = e["detectedBpm"].get<double>();
                const double error = std::abs(detected - expected);
                double best = error;
                for (const double factor : {0.5, 2.0}) best = std::min(best, std::abs(detected * factor - expected));
                errors.push_back(error); octave_errors.push_back(best);
                if (error <= tolerance) ++bpm_exact;
                if (best <= tolerance) ++bpm_octave;
            } else { errors.push_back(expected); octave_errors.push_back(expected); }   // a missed detection counts as wrong
        }
        if (e.contains("expectedKey") && e["expectedKey"].is_string()) {
            if (const auto expected = parse_key_label(e["expectedKey"].get<std::string>())) {
                ++key_total;
                const auto detected_key = e.value("detectedKeyName", json(nullptr)), detected_scale = e.value("detectedScale", json(nullptr));
                if (detected_key.is_string() && detected_scale.is_string()) {
                    const bool exact = detected_key == expected->key && detected_scale == expected->scale;
                    if (exact) ++key_exact;
                    const auto expected_code = camelot_for(expected->key, expected->scale);
                    const auto detected_code = camelot_for(detected_key.get<std::string>(), detected_scale.get<std::string>());
                    if (exact || (expected_code && detected_code && camelot_compatible(*expected_code, *detected_code))) ++key_compatible;
                }
            }
        }
    }
    const int rated = good + acceptable + bad;
    return {{"analysed", analysed}, {"failures", failed}, {"failureList", failures}, {"bpmTolerance", tolerance},
        {"bpmSamples", bpm_total}, {"exactBpmAccuracy", ratio(bpm_exact, bpm_total)}, {"octaveEquivalentBpmAccuracy", ratio(bpm_octave, bpm_total)},
        {"medianBpmError", errors.empty() ? json(nullptr) : json(median(errors))},
        {"medianBpmErrorOctaveAdjusted", octave_errors.empty() ? json(nullptr) : json(median(octave_errors))},
        {"keySamples", key_total}, {"keyAccuracy", ratio(key_exact, key_total)}, {"camelotCompatibleAccuracy", ratio(key_compatible, key_total)},
        {"averageProcessingMs", times.empty() ? json(nullptr) : json(std::accumulate(times.begin(), times.end(), 0.0) / times.size())},
        {"averageWarmProcessingMs", warm_times.empty() ? json(nullptr) : json(std::accumulate(warm_times.begin(), warm_times.end(), 0.0) / warm_times.size())},
        {"smartCue", {{"good", good}, {"acceptable", acceptable}, {"bad", bad}, {"unrated", analysed - rated},
                      {"goodRate", ratio(good, rated)}, {"goodOrAcceptableRate", ratio(good + acceptable, rated)}}}};
}

// ---- runner ------------------------------------------------------------------
MusicValidationRunner::MusicValidationRunner(fs::path root, ProviderFactory factory, std::function<bool()> low_impact)
    : root_(std::move(root)), factory_(std::move(factory)), low_impact_(std::move(low_impact)) {
    fs::create_directories(root_);
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(root_)) if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    if (files.size() > 20) files.erase(files.begin(), files.end() - 20);   // keep the 20 most recent runs
    for (const auto& file : files) {
        try {
            auto run = util::read_bounded_json(file, 64ull * 1024 * 1024);
            const auto id = run.at("id").get<std::string>();
            if (!valid_run_id(id)) continue;
            if (run["status"] == "running" || run["status"] == "queued") run["status"] = "interrupted";
            runs_[id] = std::move(run);
        } catch (...) { /* an unreadable earlier run is simply not listed */ }
    }
    worker_ = std::thread([this] { run(); });
}
MusicValidationRunner::~MusicValidationRunner() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    cancel_active_ = true;
    if (provider_) provider_->cancel_current();
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (provider_) provider_->release();
}

json MusicValidationRunner::start(const fs::path& folder, bool recursive) {
    std::error_code ec;
    if (!folder.is_absolute() || !fs::is_directory(folder, ec)) throw std::invalid_argument("folder must be an existing absolute directory");
    std::vector<fs::path> found;
    auto consider = [&](const fs::directory_entry& e) {
        std::error_code local;
        if (e.is_regular_file(local) && audio_extension(e.path())) found.push_back(e.path());
    };
    if (recursive) { for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) { consider(*it); if (found.size() > max_files) break; } }
    else for (const auto& e : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec)) { consider(e); if (found.size() > max_files) break; }
    if (found.empty()) throw std::invalid_argument("no audio files found in that folder");
    if (found.size() > max_files) throw std::length_error("folder has more than 2000 audio files; pick a smaller folder");
    std::sort(found.begin(), found.end());
    json entries = json::array();
    for (std::size_t i = 0; i < found.size(); ++i) {
        const auto relative = util::path_to_utf8(found[i].lexically_relative(folder));
        entries.push_back({{"index", i}, {"filename", util::path_to_utf8(found[i].filename())}, {"path", relative}, {"status", "pending"},
            {"expectedBpm", nullptr}, {"expectedKey", nullptr}, {"notes", ""}, {"smartCueRating", nullptr}, {"error", ""}});
    }
    std::lock_guard lock(mutex_);
    for (const auto& [id, run] : runs_) if (run["status"] == "queued" || run["status"] == "running") throw std::runtime_error("another validation run is still active");
    const auto id = "run_" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    json run = {{"id", id}, {"folder", util::path_to_utf8(folder)}, {"recursive", recursive}, {"status", "queued"}, {"createdAt", iso_now()},
                {"total", entries.size()}, {"done", 0}, {"entries", entries}, {"provider", nullptr}};
    runs_[id] = run;
    persist_locked(id);
    queue_.push_back(id); cv_.notify_one();
    return runs_[id];
}

void MusicValidationRunner::persist_locked(const std::string& id) const {
    try { util::atomic_write_json(root_ / (id + ".json"), runs_.at(id)); } catch (...) { /* the in-memory run stays authoritative */ }
}
json MusicValidationRunner::with_metrics_locked(const std::string& id) const {
    auto run = runs_.at(id);
    run["metrics"] = validation_metrics(run["entries"]);
    return run;
}
json MusicValidationRunner::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    if (!valid_run_id(id) || !runs_.contains(id)) throw std::out_of_range("validation run not found");
    return with_metrics_locked(id);
}
json MusicValidationRunner::list() const {
    std::lock_guard lock(mutex_);
    json out = json::array();
    for (auto it = runs_.rbegin(); it != runs_.rend(); ++it) {
        auto header = it->second; header.erase("entries");
        out.push_back(header);
    }
    return out;
}
bool MusicValidationRunner::cancel(const std::string& id) {
    std::shared_ptr<MusicalAnalysisProvider> provider;
    {
        std::lock_guard lock(mutex_);
        const auto it = runs_.find(id);
        if (it == runs_.end()) return false;
        if (it->second["status"] == "queued") { it->second["status"] = "cancelled"; queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end()); persist_locked(id); return true; }
        if (it->second["status"] != "running") return false;
        cancel_active_ = true; provider = provider_;
    }
    if (provider) provider->cancel_current();
    return true;
}

json MusicValidationRunner::update_entry(const std::string& id, std::size_t index, const json& fields) {
    std::lock_guard lock(mutex_);
    if (!valid_run_id(id) || !runs_.contains(id)) throw std::out_of_range("validation run not found");
    auto& entries = runs_[id]["entries"];
    if (index >= entries.size()) throw std::out_of_range("entry not found");
    json updated = entries[index];
    if (fields.contains("expectedBpm")) {
        const auto& v = fields["expectedBpm"];
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) updated["expectedBpm"] = nullptr;
        else if (v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() >= 20 && v.get<double>() <= 400) updated["expectedBpm"] = v.get<double>();
        else throw std::invalid_argument("expectedBpm must be between 20 and 400");
    }
    if (fields.contains("expectedKey")) {
        const auto& v = fields["expectedKey"];
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) { updated["expectedKey"] = nullptr; updated.erase("expectedKeyNormalized"); }
        else if (v.is_string() && v.get<std::string>().size() <= 24) {
            const auto parsed = parse_key_label(v.get<std::string>());
            if (!parsed) throw std::invalid_argument("expectedKey not understood (try \"F minor\", \"C#m\" or \"8A\")");
            updated["expectedKey"] = v.get<std::string>(); updated["expectedKeyNormalized"] = parsed->key + " " + parsed->scale;
        } else throw std::invalid_argument("expectedKey must be a short string");
    }
    if (fields.contains("notes")) {
        if (!fields["notes"].is_string() || fields["notes"].get<std::string>().size() > 500) throw std::invalid_argument("notes must be text up to 500 characters");
        updated["notes"] = fields["notes"];
    }
    if (fields.contains("smartCueRating")) {
        const auto& v = fields["smartCueRating"];
        if (v.is_null() || v == "") updated["smartCueRating"] = nullptr;
        else if (v == "Good" || v == "Acceptable" || v == "Bad") updated["smartCueRating"] = v;
        else throw std::invalid_argument("smartCueRating must be Good, Acceptable or Bad");
    }
    entries[index] = updated;
    persist_locked(id);
    return {{"entry", updated}, {"metrics", validation_metrics(entries)}};
}

void MusicValidationRunner::run() {
    while (true) {
        std::string id;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            id = queue_.front(); queue_.pop_front();
            if (runs_[id]["status"] != "queued") continue;
            runs_[id]["status"] = "running"; runs_[id]["startedAt"] = iso_now(); active_ = id; cancel_active_ = false;
            persist_locked(id);
        }
        try { process(id); }
        catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            runs_[id]["status"] = "failed"; runs_[id]["error"] = e.what(); runs_[id]["finishedAt"] = iso_now(); persist_locked(id);
        }
        std::shared_ptr<MusicalAnalysisProvider> provider;
        { std::lock_guard lock(mutex_); active_.clear(); provider = provider_; }
        if (provider) provider->release();   // do not keep the Python stack resident after the run
    }
}

void MusicValidationRunner::process(const std::string& id) {
    std::shared_ptr<MusicalAnalysisProvider> provider;
    try { provider = factory_(); } catch (const std::exception& e) { throw std::runtime_error(std::string("musical analysis unavailable: ") + e.what()); }
    if (!provider) throw std::runtime_error("musical analysis unavailable");
    { std::lock_guard lock(mutex_); provider_ = provider; runs_[id]["provider"] = {{"id", provider->id()}, {"version", provider->provider_version()}}; }
    json folder_json, entries_snapshot;
    { std::lock_guard lock(mutex_); folder_json = runs_[id]["folder"]; entries_snapshot = runs_[id]["entries"]; }
    const auto folder = util::utf8_to_path(folder_json.get<std::string>());
    library::RuleBasedCueSuggestionProvider cues;
    std::size_t done = 0;
    for (std::size_t i = 0; i < entries_snapshot.size(); ++i) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || cancel_active_) break;
        }
        while (low_impact_ && low_impact_()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::lock_guard lock(mutex_);
            if (stopping_ || cancel_active_) break;
        }
        if (cancel_active_ || stopping_) break;
        json update = {{"status", "done"}, {"error", ""}};
        try {
            const auto path = folder / util::utf8_to_path(entries_snapshot[i]["path"].get<std::string>());
            const auto hash = library::hash_asset(path);
            AnalysisJob job; job.id = "validation"; job.track_id = "track_" + hash.substr(hash.find(':') + 1);
            job.content_hash = hash; job.provider = provider->id(); job.asset_path = util::path_to_utf8(path);
            const auto begin = std::chrono::steady_clock::now();
            auto result = provider->execute(job);
            update["analysisDurationMs"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            const auto& doc = result.at("musical");
            update["durationMs"] = doc.value("durationMs", json(nullptr));
            update["detectedBpm"] = doc.value("bpm", json(nullptr)); update["bpmConfidence"] = doc.value("bpmConfidence", json(nullptr));
            update["bpmConfidenceLabel"] = doc.value("bpmConfidenceLabel", json(nullptr)); update["bpmAmbiguous"] = doc.value("bpmAmbiguous", false);
            update["alternatives"] = doc.value("bpmAlternatives", json::array());
            update["detectedKeyName"] = doc.value("key", json(nullptr)); update["detectedScale"] = doc.value("scale", json(nullptr));
            update["detectedKey"] = doc["key"].is_string() ? json(doc["key"].get<std::string>() + " " + doc["scale"].get<std::string>()) : json(nullptr);
            update["keyConfidence"] = doc.value("keyConfidence", json(nullptr)); update["camelot"] = doc.value("camelot", json(nullptr));
            update["energy"] = doc.value("energy", json(nullptr)); update["sections"] = doc.value("sections", json::array());
            update["warnings"] = doc.value("warnings", json::array());
            if (doc.contains("timingsMs")) update["timingsMs"] = doc["timingsMs"];
            library::CueSuggestionInput input; input.duration_ms = static_cast<std::int64_t>(doc.value("durationMs", 0.0)); input.musical = consumer_view(doc);
            json suggested = json::array();
            for (const auto& s : cues.suggest(input).suggestions) suggested.push_back({{"intent", s.intent}, {"startMs", s.start_ms}, {"endMs", s.end_ms}, {"confidence", s.confidence}});
            update["smartCue"] = suggested;
        } catch (const std::exception& e) {
            if (cancel_active_) { update = {{"status", "pending"}, {"error", ""}}; }
            else update = {{"status", "failed"}, {"error", e.what()}};
        }
        std::lock_guard lock(mutex_);
        auto& entry = runs_[id]["entries"][i];
        for (const auto& [name, value] : update.items()) entry[name] = value;
        if (update["status"] != "pending") ++done;
        runs_[id]["done"] = done;
        if (done % 5 == 0 || i + 1 == entries_snapshot.size()) persist_locked(id);
    }
    std::lock_guard lock(mutex_);
    runs_[id]["status"] = cancel_active_ ? "cancelled" : (stopping_ ? "interrupted" : "completed");
    runs_[id]["finishedAt"] = iso_now(); runs_[id]["done"] = done;
    persist_locked(id);
}

std::string MusicValidationRunner::export_json(const std::string& id) const { return get(id).dump(2); }

std::string MusicValidationRunner::export_csv(const std::string& id) const {
    const auto run = get(id);
    const auto cell = [](const json& v) {
        std::string text = v.is_null() ? "" : v.is_string() ? v.get<std::string>() : v.is_number_float() ? [&] { std::ostringstream o; o << std::defaultfloat << v.get<double>(); return o.str(); }() : v.dump();
        if (!text.empty() && (text[0] == '=' || text[0] == '+' || text[0] == '-' || text[0] == '@') && !v.is_number()) text = "'" + text;   // no spreadsheet formulas from file names or notes
        if (text.find_first_of(",\"\r\n") != std::string::npos) {
            std::string quoted = "\"";
            for (char c : text) { if (c == '"') quoted += '"'; quoted += c; }
            return quoted + "\"";
        }
        return text;
    };
    const auto clock = [](double ms) { const long total = static_cast<long>(ms / 1000.0); char b[24]; std::snprintf(b, sizeof b, "%ld:%02ld", total / 60, total % 60); return std::string(b); };
    std::ostringstream out;
    out << "filename,duration,detectedBpm,bpmConfidence,alternatives,detectedKey,keyConfidence,camelot,energy,sections,analysisDurationMs,errors,expectedBpm,expectedKey,notes,smartCueRating,smartCueSuggestions\n";
    for (const auto& e : run["entries"]) {
        std::string alternatives, sections, suggestions;
        for (const auto& a : e.value("alternatives", json::array())) alternatives += (alternatives.empty() ? "" : "; ") + a.value("relation", std::string()) + " " + std::to_string(a.value("bpm", 0.0)).substr(0, 6);
        for (const auto& s : e.value("sections", json::array())) sections += (sections.empty() ? "" : "; ") + s.value("type", std::string()) + " " + clock(s.value("startMs", 0.0)) + "-" + clock(s.value("endMs", 0.0));
        for (const auto& s : e.value("smartCue", json::array())) suggestions += (suggestions.empty() ? "" : "; ") + s.value("intent", std::string()) + " " + clock(s.value("startMs", 0.0)) + "-" + clock(s.value("endMs", 0.0));
        const json duration = e.contains("durationMs") && e["durationMs"].is_number() ? json(e["durationMs"].get<double>() / 1000.0) : json(nullptr);
        const json fields[] = {e.value("filename", std::string()), duration, e.value("detectedBpm", json(nullptr)), e.value("bpmConfidence", json(nullptr)), alternatives,
            e.value("detectedKey", json(nullptr)), e.value("keyConfidence", json(nullptr)), e.value("camelot", json(nullptr)), e.value("energy", json(nullptr)), sections,
            e.value("analysisDurationMs", json(nullptr)), e.value("error", std::string()), e.value("expectedBpm", json(nullptr)), e.value("expectedKey", json(nullptr)),
            e.value("notes", std::string()), e.value("smartCueRating", json(nullptr)), suggestions};
        for (std::size_t i = 0; i < std::size(fields); ++i) out << (i ? "," : "") << cell(fields[i]);
        out << "\n";
    }
    return out.str();
}

} // namespace liveplay::analysis
