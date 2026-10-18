// Phase 2 (Smart Music Engine) native tests: musical model, Camelot, store,
// stale handling, cue suggestions and scheduler integration (cancel / crash /
// missing provider). Worker-level DSP quality is covered by tests/mir/test_worker.py.
#include "liveplay/analysis/musical.hpp"
#include "liveplay/analysis/validation_runner.hpp"
#include "liveplay/library/cue_suggestions.hpp"
#include "liveplay/library/maintenance.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sodium.h>
#include <thread>

namespace fs = std::filesystem;
using namespace liveplay;
using json = nlohmann::json;
namespace {
int failures = 0;
void check(bool value, const char* text) { std::printf("%s %s\n", value ? "PASS" : "FAIL", text); if (!value) ++failures; }
template <class F> bool throws(F fn) { try { fn(); return false; } catch (...) { return true; } }

std::string track_id(char c) { return "track_" + std::string(64, c); }

json sample_document(const std::string& hash, double bpm = 128.0) {
    json beats = json::array(); const double interval = 60000.0 / bpm;
    for (double t = 250.0; t < 155000.0; t += interval) beats.push_back(t);
    json curve = json::array();
    for (int s = 0; s < 155; ++s) curve.push_back(s < 18 ? 0.3 : s < 42 ? 0.4 + 0.4 * (s - 18) / 24.0 : s < 72 ? 0.95 : s < 98 ? 0.35 : s < 128 ? 0.95 : 0.3);
    return {{"schemaVersion", 1}, {"provider", "librosa-mir"}, {"providerVersion", "librosa 0.11.0"},
        {"analyzedAt", "2026-10-18T10:00:00Z"}, {"sourceHash", hash}, {"durationMs", 155000.0},
        {"bpm", bpm}, {"bpmConfidence", 0.96}, {"bpmAmbiguous", false},
        {"beatsMs", beats}, {"downbeatsMs", json::array()},
        {"beatGrid", {{"firstBeatMs", 250.0}, {"intervalMs", interval}, {"tempo", bpm}, {"confidence", 0.96}, {"constantTempo", true},
                      {"segments", json::array({{{"startMs", 250.0}, {"endMs", 154900.0}, {"bpm", bpm}, {"beatCount", beats.size()}}})}}},
        {"key", "F"}, {"scale", "minor"}, {"camelot", "9B" /* deliberately wrong */}, {"keyConfidence", 0.9},
        {"energy", 0.94}, {"energyConfidence", 0.8}, {"energyCurve", {{"hopMs", 1000}, {"values", curve}}},
        {"sections", json::array({
            {{"type", "intro"}, {"startMs", 0}, {"endMs", 18000}, {"confidence", 0.8}},
            {{"type", "build"}, {"startMs", 18000}, {"endMs", 42000}, {"confidence", 0.7}},
            {{"type", "drop"}, {"startMs", 42000}, {"endMs", 72000}, {"confidence", 0.9}},
            {{"type", "break"}, {"startMs", 72000}, {"endMs", 98000}, {"confidence", 0.7}},
            {{"type", "drop"}, {"startMs", 98000}, {"endMs", 128000}, {"confidence", 0.9}},
            {{"type", "outro"}, {"startMs", 128000}, {"endMs", 155000}, {"confidence", 0.8}}})},
        {"capabilities", {{"bpm", true}, {"downbeats", false}}}};
}

void camelot_tests() {
    // Exhaustive: all 24 keys against the published Camelot wheel.
    const char* names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const int major[12] = {8,3,10,5,12,7,2,9,4,11,6,1}, minor[12] = {5,12,7,2,9,4,11,6,1,8,3,10};
    bool all = true;
    for (int i = 0; i < 12; ++i) {
        all &= analysis::camelot_for(names[i], "major") == std::to_string(major[i]) + "B";
        all &= analysis::camelot_for(names[i], "minor") == std::to_string(minor[i]) + "A";
    }
    check(all, "Camelot: all 24 keys map to the published wheel");
    check(analysis::camelot_for("F", "minor") == "4A" && analysis::camelot_for("C", "major") == "8B", "Camelot: F minor = 4A, C major = 8B");
    check(analysis::camelot_for("C#", "minor") == "12A" && analysis::camelot_for("A", "minor") == "8A" && analysis::camelot_for("B", "major") == "1B", "Camelot: C# minor 12A, A minor 8A, B major 1B");
    bool relatives = true, fifths = true;
    for (int i = 0; i < 12; ++i) {
        // Relative minor (3 semitones down) shares the number with its major.
        relatives &= major[i] == minor[(i + 9) % 12];
        // A perfect fifth up is one step clockwise on the wheel (mod 12) in both rings.
        fifths &= (major[(i + 7) % 12] - major[i] + 12) % 12 == 1 && (minor[(i + 7) % 12] - minor[i] + 12) % 12 == 1;
    }
    check(relatives, "Camelot: relative major/minor share their wheel number");
    check(fifths, "Camelot: a fifth up advances one position in both rings");
    check(analysis::camelot_for("Db", "major") == "3B" && analysis::camelot_for("Bb", "minor") == "3A" && analysis::camelot_for("A\xE2\x99\xAD", "major") == "4B", "Camelot: flats and unicode flats are enharmonic");
    check(analysis::camelot_for("c#", "MINOR") == "12A" && analysis::camelot_for("F", "min") == "4A", "Camelot: case and abbreviations tolerated");
    check(analysis::camelot_for("E#", "major") == "7B" && analysis::camelot_for("Cb", "major") == "1B", "Camelot: E# is F and Cb is B");
    check(!analysis::camelot_for("H", "major") && !analysis::camelot_for("C", "dorian") && !analysis::camelot_for("", "minor") && !analysis::camelot_for("C##", "major"), "Camelot: invalid input yields no value");
    check(analysis::normalize_key_name("  g# ") == "G#" && analysis::normalize_key_name("Gb") == "F#", "key names normalise to sharp spelling");
    check(analysis::energy_level(0.0) == 0 && analysis::energy_level(0.01) == 1 && analysis::energy_level(0.2) == 2 && analysis::energy_level(0.5) == 3 &&
          analysis::energy_level(0.79) == 4 && analysis::energy_level(0.8) == 5 && analysis::energy_level(1.0) == 5 && analysis::energy_level(NAN) == 0, "energy 0..1 maps to UI levels 1..5");
    check(analysis::confidence_label(0.96) == "high" && analysis::confidence_label(0.6) == "medium" && analysis::confidence_label(0.2) == "low", "confidence labels");
}

void validation_tests() {
    const std::string hash = "sha256:" + std::string(64, 'a');
    auto valid = analysis::validate_musical(sample_document(hash));
    check(valid["camelot"] == "4A", "provider Camelot is ignored and recomputed (F minor = 4A)");
    check(valid["energyLevel"] == 5 && valid["bpmConfidenceLabel"] == "high" && valid["stale"] == false, "derived fields: energy level, confidence label, stale=false");
    check(valid["downbeatsAvailable"] == false && valid["downbeatsMs"].empty(), "no downbeats are invented");
    check(valid["corrections"].is_object() && valid["corrections"].empty(), "reserved manual-corrections slot exists and is empty");

    auto mutate = [&](auto fn) { auto d = sample_document(hash); fn(d); return d; };
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["schemaVersion"] = 2; })); }), "unknown schema version rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["sourceHash"] = "md5:abc"; })); }), "source hash must be sha256");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d.erase("sourceHash"); })); }), "source hash is mandatory");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["bpm"] = 900; })); }), "BPM out of range rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["bpmConfidence"] = 1.5; })); }), "confidence above 1 rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["beatsMs"] = json::array({100.0, 90.0}); })); }), "beats must be strictly increasing");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["beatsMs"] = json::array({-5.0, 10.0}); })); }), "negative beat rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["sections"][2]["startMs"] = 30000; })); }), "overlapping sections rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["sections"][0]["endMs"] = 0; })); }), "empty section rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["key"] = "Z"; })); }), "unrecognised key rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["energy"] = -0.1; })); }), "negative energy rejected");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["energyCurve"]["values"][0] = 2.0; })); }), "energy curve value above 1 rejected");
    check(throws([&] { analysis::validate_musical(json::array()); }), "non-object rejected");
    auto odd = analysis::validate_musical(mutate([](json& d) { d["sections"][1]["type"] = "psychedelic"; }));
    check(odd["sections"][1]["type"] == "unknown", "provider-specific section types degrade to unknown");
    auto minimal = analysis::validate_musical({{"schemaVersion", 1}, {"provider", "p"}, {"analyzedAt", "2026-01-01T00:00:00Z"}, {"sourceHash", hash}});
    check(minimal["bpm"].is_null() && minimal["key"].is_null() && minimal["camelot"].is_null() && minimal["sections"].empty() && minimal["beatGrid"].is_null(),
          "a document with nothing detected is valid and fully null (silence / too short)");
    // Tempo changes need no model change: an ordered multi-segment grid is accepted.
    auto changing = analysis::validate_musical(mutate([](json& d) {
        d["beatGrid"]["constantTempo"] = false;
        d["beatGrid"]["segments"] = json::array({{{"startMs", 0}, {"endMs", 30000}, {"bpm", 120}}, {{"startMs", 30000}, {"endMs", 60000}, {"bpm", 140}}}); }));
    check(changing["beatGrid"]["segments"].size() == 2 && changing["beatGrid"]["constantTempo"] == false, "beat grid supports tempo changes");
    check(throws([&] { analysis::validate_musical(mutate([](json& d) { d["beatGrid"]["segments"] = json::array({{{"startMs", 0}, {"endMs", 30000}, {"bpm", 120}}, {{"startMs", 10000}, {"endMs", 60000}, {"bpm", 140}}}); })); }),
          "overlapping beat grid segments rejected");
}

void store_tests(const fs::path& root) {
    const auto dir = root / "store";
    const std::string hash = "sha256:" + std::string(64, 'a');
    {
        analysis::MusicalAnalysisStore store(dir);
        check(store.state(track_id('1'), hash) == "absent" && !store.find(track_id('1')), "absent analysis is reported as absent");
        store.save(track_id('1'), sample_document(hash));
        check(store.state(track_id('1'), hash) == "ready", "saved analysis is ready for the matching source hash");
        check(store.state(track_id('1'), "sha256:" + std::string(64, 'b')) == "stale", "a different source hash makes the analysis stale");
        auto found = store.find(track_id('1'));
        check(found && (*found)["bpm"] == 128.0 && (*found)["camelot"] == "4A" && (*found)["trackId"] == track_id('1'), "stored document round-trips");
        store.mark_stale(track_id('1'));
        check(store.state(track_id('1'), hash) == "stale", "mark_stale flags the analysis even when the hash matches");
        found = store.find(track_id('1'));
        check(found && (*found)["stale"] == true && (*found)["bpm"] == 128.0, "stale data is kept, not deleted");
        store.save(track_id('1'), sample_document(hash, 126.0));
        check(store.state(track_id('1'), hash) == "ready" && (*store.find(track_id('1')))["bpm"] == 126.0, "re-analysis replaces stale data");
        store.save(track_id('2'), sample_document(hash));
        check(store.summaries().size() == 2 && !store.summaries()[track_id('1')].contains("beatsMs"), "summaries are compact (no beat arrays)");
        check(throws([&] { store.save("../escape", sample_document(hash)); }) && throws([&] { store.find("track_xyz"); }), "track IDs are validated");
        check(throws([&] { auto bad = sample_document(hash); bad["bpm"] = 1000; store.save(track_id('3'), bad); }) && store.state(track_id('3'), hash) == "absent", "invalid documents never reach disk");
    }
    {
        analysis::MusicalAnalysisStore reopened(dir);
        check(reopened.summaries().size() == 2 && reopened.state(track_id('2'), hash) == "ready", "analysis survives restart");
    }
    fs::remove(dir / "index.json");
    { std::ofstream corrupt(dir / (track_id('4') + ".json")); corrupt << "{not json"; }
    {
        analysis::MusicalAnalysisStore rebuilt(dir);
        check(rebuilt.summaries().size() == 2 && rebuilt.state(track_id('4'), hash) == "absent", "missing index is rebuilt from documents; corrupt documents are skipped");
        rebuilt.remove(track_id('2'));
        check(rebuilt.state(track_id('2'), hash) == "absent" && !fs::exists(dir / (track_id('2') + ".json")), "remove deletes document and index entry");
    }
}

// ---- scheduler integration --------------------------------------------------
class FakeMusicalProvider final : public analysis::MusicalAnalysisProvider {
public:
    enum class Mode { ok, crash, block, mutate_asset } mode = Mode::ok;
    fs::path mutate_path;
    std::atomic<bool> cancelled{false};
    std::atomic<int> runs{0};
    std::string id() const override { return "fake-mir"; }
    json capabilities() const override { return {{"rhythm", true}, {"tonality", true}, {"structure", true}}; }
    json musical_capabilities() const override { return {{"bpm", true}}; }
    json execute(const analysis::AnalysisJob& job) override {
        ++runs;
        if (mode == Mode::crash) throw std::runtime_error("analysis worker terminated unexpectedly");
        if (mode == Mode::block) { cancelled = false; while (!cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(5)); throw std::runtime_error("analysis worker cancelled"); }
        if (mode == Mode::mutate_asset) { std::ofstream(mutate_path, std::ios::app) << "changed"; }
        auto result = analysis::empty_analysis(job.track_id);
        auto doc = sample_document(job.content_hash); doc["analyzedAt"] = "2026-10-18T10:00:0" + std::to_string(runs % 10) + "Z";
        result["musical"] = analysis::validate_musical(doc);
        return result;
    }
    void cancel_current() noexcept override { cancelled = true; }
};

json wait_job(analysis::AnalysisScheduler& scheduler, const std::string& id, const std::set<std::string>& states) {
    for (int i = 0; i < 1000; ++i) {
        if (const auto job = scheduler.job(id, "fake-mir"); job && states.contains(job->status)) return json(*job);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return json{{"status", "timeout"}};
}
library::Track make_track(const fs::path& root, char c, const std::string& content) {
    library::Track t; t.id = track_id(c); t.title = "T"; t.format = "wav";
    const auto file = root / (std::string(1, c) + ".bin");
    { std::ofstream out(file, std::ios::binary); out << content; }
    t.asset_path = util::path_to_utf8(file); t.content_hash = library::hash_asset(file); t.duration_ms = 155000;
    return t;
}

void scheduler_tests(const fs::path& root) {
    fs::create_directories(root / "sched" / "analysis");
    analysis::JsonTrackAnalysisRepository repository(root / "sched" / "analysis" / "tracks");
    analysis::MusicalAnalysisStore store(root / "sched" / "analysis" / "musical");
    auto provider = std::make_shared<FakeMusicalProvider>();
    analysis::AnalysisScheduler scheduler(repository, root / "sched" / "analysis", {provider});
    scheduler.set_sink("fake-mir", {[&](const analysis::AnalysisJob& job, json& result) { store.save(job.track_id, result.at("musical")); }, nullptr});

    const auto a = make_track(root, 'a', "alpha"), b = make_track(root, 'b', "beta"), c = make_track(root, 'c', "gamma");
    scheduler.enqueue(a, "fake-mir");
    check(wait_job(scheduler, a.id, {"completed"})["status"] == "completed" && store.state(a.id, a.content_hash) == "ready", "job completes and result lands in the musical store");
    check(!repository.find(a.id), "musical result does not overwrite the loudness/basic analysis record");
    const auto before = provider->runs.load();
    scheduler.enqueue(a, "fake-mir");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(provider->runs == before, "an already-completed analysis is not repeated without force");
    scheduler.enqueue(a, "fake-mir", true);
    for (int i = 0; i < 400 && provider->runs == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(provider->runs == before + 1, "force re-analyzes (Reanalyze)");
    wait_job(scheduler, a.id, {"completed"});

    // Provider crash is isolated: job fails, previous good data remains.
    provider->mode = FakeMusicalProvider::Mode::crash;
    scheduler.enqueue(a, "fake-mir", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto failed = wait_job(scheduler, a.id, {"failed"});
    check(failed["status"] == "failed" && failed["error"].get<std::string>().find("terminated") != std::string::npos, "worker crash fails only that job");
    check(store.state(a.id, a.content_hash) == "ready" && (*store.find(a.id))["bpm"] == 128.0, "a failed re-analysis leaves the previous result intact");

    // Cancellation of a running and a pending job.
    provider->mode = FakeMusicalProvider::Mode::block;
    scheduler.enqueue(b, "fake-mir"); scheduler.enqueue(c, "fake-mir");
    for (int i = 0; i < 400 && scheduler.job(b.id, "fake-mir")->status != "running"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(scheduler.job(c.id, "fake-mir")->status == "pending", "second job waits behind the running one (single worker)");
    check(scheduler.cancel({}, "fake-mir") == 2, "Cancel Analysis reports running + pending jobs");
    check(wait_job(scheduler, b.id, {"cancelled"})["status"] == "cancelled" && scheduler.job(c.id, "fake-mir")->status == "cancelled", "cancelled jobs end as cancelled, not failed");
    check(store.state(b.id, b.content_hash) == "absent" && store.state(c.id, c.content_hash) == "absent", "cancelled jobs store nothing");
    provider->mode = FakeMusicalProvider::Mode::ok;
    scheduler.enqueue(b, "fake-mir");
    check(wait_job(scheduler, b.id, {"completed"})["status"] == "completed", "a cancelled job can be queued again");

    // The source changing during analysis invalidates the result.
    provider->mode = FakeMusicalProvider::Mode::mutate_asset; provider->mutate_path = util::utf8_to_path(c.asset_path);
    scheduler.enqueue(c, "fake-mir", true);
    const auto changed = wait_job(scheduler, c.id, {"failed", "completed"});
    check(changed["status"] == "failed" && store.state(c.id, c.content_hash) == "absent", "asset changed during analysis is rejected");
    provider->mode = FakeMusicalProvider::Mode::ok;

    // Missing provider degrades to a failed job; nothing else is affected.
    library::Track d = make_track(root, 'd', "delta");
    scheduler.enqueue(d, "does-not-exist");
    for (int i = 0; i < 400 && scheduler.job(d.id, "does-not-exist")->status != "failed"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(scheduler.job(d.id, "does-not-exist")->status == "failed" && scheduler.job(d.id, "does-not-exist")->error.find("unavailable") != std::string::npos, "unavailable provider fails the job explicitly");
    check(throws([&] { analysis::ExternalMusicalProvider missing(root / "no-such-worker.exe", analysis::ExternalWorkerOptions{}); }), "a missing worker executable is reported at construction");

    // Performance Mode pauses work while playback is active and resumes afterwards.
    std::atomic<bool> playing{true};
    analysis::AnalysisScheduler guarded(repository, root / "sched" / "guarded", {provider}, [&] { return playing.load(); });
    guarded.set_sink("fake-mir", {[&](const analysis::AnalysisJob& job, json& result) { store.save(job.track_id, result.at("musical")); }, nullptr});
    const auto e = make_track(root, 'e', "epsilon");
    const auto runs_before = provider->runs.load();
    guarded.enqueue(e, "fake-mir");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    check(provider->runs == runs_before && guarded.job(e.id, "fake-mir")->status == "running", "analysis is held back while playback is protected");
    playing = false;
    check(wait_job(guarded, e.id, {"completed"})["status"] == "completed", "analysis resumes when playback stops");
    // Cancelling while held back (before the worker started) must also work.
    playing = true;
    const auto f = make_track(root, 'f', "zeta");
    guarded.enqueue(f, "fake-mir");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(guarded.cancel({f.id}, "fake-mir") == 1, "a paused job can be cancelled");
    playing = false;
    check(wait_job(guarded, f.id, {"cancelled"})["status"] == "cancelled", "paused job ends cancelled");
}

// ---- cue suggestions --------------------------------------------------------
const library::CueSuggestion* find_suggestion(const library::CueSuggestionSet& set, const std::string& intent) {
    for (const auto& s : set.suggestions) if (s.intent == intent) return &s;
    return nullptr;
}
bool on_grid(double ms, double bpm, double offset = 250.0) {
    const double interval = 60000.0 / bpm, k = std::round((ms - offset) / interval);
    return std::abs(ms - (offset + k * interval)) < 1.5;
}
void cue_tests() {
    library::RuleBasedCueSuggestionProvider provider;
    check(provider.intents().size() == 5, "five generic intents are registered");
    const std::string hash = "sha256:" + std::string(64, 'a');
    const auto musical = analysis::validate_musical(sample_document(hash));
    library::CueSuggestionInput input; input.duration_ms = 155000; input.musical = musical;
    const auto set = provider.suggest(input);
    const auto* strong = find_suggestion(set, "strong-start");
    check(strong && strong->start_ms >= 38000 && strong->start_ms < 42000 && on_grid(static_cast<double>(strong->start_ms), 128.0) &&
          std::abs(strong->end_ms - 72000) < 500 && strong->confidence > 0.75, "Strong Start begins about one bar before the drop, on the beat grid, and runs to its end");
    check(strong && std::find(strong->basis.begin(), strong->basis.end(), "sections") != strong->basis.end() && std::find(strong->basis.begin(), strong->basis.end(), "beats") != strong->basis.end(), "suggestion reports which inputs it used");
    const auto* outro = find_suggestion(set, "clean-outro");
    check(outro && std::abs(outro->start_ms - 128000) < 500 && outro->end_ms == 155000 && outro->fade_out_ms >= 2500, "Clean Outro starts at the outro section and fades out");
    const auto* section = find_suggestion(set, "section-start");
    check(section && section->start_ms >= 0 && section->end_ms > section->start_ms, "Section Start picks a detected section");
    const auto* energetic = find_suggestion(set, "high-energy");
    check(energetic && energetic->end_ms - energetic->start_ms >= 10000, "High Energy returns a bounded stretch");
    bool ordered = !set.suggestions.empty();
    for (const auto& s : set.suggestions) ordered &= s.start_ms >= 0 && s.end_ms <= 155000 && s.start_ms < s.end_ms && s.confidence >= 0.0 && s.confidence <= 1.0 && !s.reason.empty();
    check(ordered, "every suggestion lies inside the track with ordered IN/OUT, a confidence and a reason");
    for (const auto& s : set.suggestions) if (s.intent == "steady-background") check(s.start_ms >= 18000 && s.end_ms <= 128500, "Steady Background avoids the intro and outro");
    check(find_suggestion(provider.suggest(input, {"strong-start"}), "clean-outro") == nullptr, "requested intents are honoured");
    check(provider.suggest(input, {"no-such-intent"}).unavailable.size() == 1, "unknown intent is reported, not guessed");

    // Unreliable tempo: positions are still suggested but never snapped to a dubious grid.
    auto shaky = musical; shaky["bpmConfidence"] = 0.2;
    library::CueSuggestionInput unsure = input; unsure.musical = shaky;
    const auto* free_start = find_suggestion(provider.suggest(unsure, {"strong-start"}), "strong-start");
    check(free_start && std::find(free_start->basis.begin(), free_start->basis.end(), "beats") == free_start->basis.end(), "low-confidence BPM is not used for grid snapping");

    // Waveform-only fallback: quiet first half, loud second half.
    library::CueSuggestionInput wave; wave.duration_ms = 120000;
    for (int i = 0; i < 600; ++i) wave.waveform_rms.push_back(i < 300 ? 0.05f : 0.5f);
    const auto wave_set = provider.suggest(wave, {"strong-start", "clean-outro"});
    const auto* wave_start = find_suggestion(wave_set, "strong-start");
    check(wave_start && wave_start->start_ms >= 50000 && wave_start->start_ms <= 60000 &&
          std::find(wave_start->basis.begin(), wave_start->basis.end(), "waveform") != wave_start->basis.end(), "waveform alone still yields a Strong Start (no musical analysis needed)");
    // Flat or empty material gives no invented suggestion.
    library::CueSuggestionInput flat; flat.duration_ms = 120000; flat.waveform_rms.assign(600, 0.3f);
    check(provider.suggest(flat).suggestions.empty() && provider.suggest(flat).unavailable.size() == 5, "flat material yields no suggestions rather than invented ones");
    library::CueSuggestionInput nothing; nothing.duration_ms = 120000;
    check(provider.suggest(nothing).suggestions.empty(), "no data yields no suggestions");
    // Custom intents plug in.
    provider.register_intent({"first-ten-seconds", "First Ten Seconds", "test"}, [](const library::CueSuggestionContext& c) -> std::optional<library::CueSuggestion> {
        library::CueSuggestion s; s.intent = "first-ten-seconds"; s.label = "First Ten Seconds"; s.start_ms = 0; s.end_ms = std::min<std::int64_t>(10000, c.duration_ms); s.confidence = 0.5; s.reason = "custom"; return s; });
    check(find_suggestion(provider.suggest(input, {"first-ten-seconds"}), "first-ten-seconds") != nullptr && provider.intents().size() == 6, "custom intents can be registered");
}

// ---- manual corrections (Phase 2.5) ----------------------------------------
void corrections_tests(const fs::path& root) {
    const std::string hash = "sha256:" + std::string(64, 'a');
    auto doc = analysis::validate_musical(sample_document(hash));
    const auto with = [&](json corrections) { auto d = doc; d["corrections"] = analysis::validate_corrections(corrections, d); return d; };

    auto plain = analysis::effective_values(doc);
    check(plain["bpm"] == 128.0 && plain["bpmSource"] == "detected" && plain["gridSource"] == "detected" && plain["keySource"] == "detected" &&
          plain["corrected"] == false && plain["camelot"] == "4A", "no corrections: effective values equal detected");
    check(analysis::effective_beats(doc).size() == doc["beatsMs"].size(), "no corrections: detected beats are used");

    auto doubled = with({{"bpm", {{"mode", "factor"}, {"value", 2.0}}}});
    auto e = analysis::effective_values(doubled);
    check(e["bpm"] == 256.0 && e["bpmSource"] == "factor" && e["bpmConfidence"] == 1.0 && e["bpmAmbiguous"] == false && e["gridSource"] == "corrected" &&
          std::abs(e["beatGrid"]["intervalMs"].get<double>() - 60000.0 / 256.0) < 1e-6, "BPM x2: effective 256, grid follows the new tempo");
    auto halved = analysis::effective_values(with({{"bpm", {{"mode", "factor"}, {"value", 0.5}}}}));
    check(halved["bpm"] == 64.0 && halved["bpmSource"] == "factor", "BPM /2: effective 64");
    auto custom = analysis::effective_values(with({{"bpm", {{"mode", "custom"}, {"value", 127.4}}}}));
    check(custom["bpm"] == 127.4 && custom["bpmSource"] == "custom", "custom BPM");
    check(doubled["bpm"] == 128.0 && doubled["beatsMs"] == doc["beatsMs"] && doubled["beatGrid"] == doc["beatGrid"], "the detected analysis is never rewritten by corrections");

    check(throws([&] { with({{"bpm", {{"mode", "custom"}, {"value", 10.0}}}}); }) && throws([&] { with({{"bpm", {{"mode", "factor"}, {"value", 50.0}}}}); }) &&
          throws([&] { with({{"bpm", {{"mode", "weird"}, {"value", 100.0}}}}); }) && throws([&] { with({{"bpm", {{"mode", "factor"}}}}); }), "invalid BPM corrections rejected");
    auto silent = analysis::validate_musical({{"schemaVersion", 1}, {"provider", "p"}, {"analyzedAt", "2026-01-01T00:00:00Z"}, {"sourceHash", hash}, {"durationMs", 30000}});
    check(throws([&] { analysis::validate_corrections({{"bpm", {{"mode", "factor"}, {"value", 2.0}}}}, silent); }), "a factor needs a detected BPM");
    check(throws([&] { analysis::validate_corrections({{"firstBeatMs", 100.0}}, silent); }) && !throws([&] { analysis::validate_corrections({{"firstBeatMs", 100.0}, {"bpm", {{"mode", "custom"}, {"value", 90.0}}}}, silent); }),
          "a grid correction on an undetected track needs a BPM");
    auto manual_doc = silent; manual_doc["corrections"] = analysis::validate_corrections({{"firstBeatMs", 100.0}, {"bpm", {{"mode", "custom"}, {"value", 90.0}}}}, silent);
    auto manual = analysis::effective_values(manual_doc);
    check(manual["bpm"] == 90.0 && manual["beatGrid"]["firstBeatMs"] == 100.0, "an operator can supply tempo and grid for a track the detector gave up on");

    auto shifted = with({{"firstBeatMs", 1000.0}, {"gridOffsetMs", -25.0}});
    const auto beats = analysis::effective_beats(shifted);
    bool spaced = beats.size() > 300; for (std::size_t i = 1; i < beats.size(); ++i) spaced &= std::abs(beats[i] - beats[i - 1] - 468.75) < 1e-6;
    check(spaced && beats.front() >= 0 && beats.front() < 468.75 && beats.back() <= 155000.0, "corrected beats are a regular grid inside the track");
    // 975 mod 468.75 = 37.5: first beat + offset define the phase.
    check(std::abs(beats.front() - 37.5) < 1e-6, "first beat + offset define the grid phase");
    const auto grid_first = [&](double offset) { return analysis::effective_values(with({{"gridOffsetMs", offset}, {"bpm", {{"mode", "custom"}, {"value", 128.0}}}}))["beatGrid"]["firstBeatMs"].get<double>(); };
    const double move = grid_first(10.0) - grid_first(0.0);
    check(std::abs(move - 10.0) < 1e-6 || std::abs(move + 468.75 - 10.0) < 1e-6, "moving the grid right by 10 ms shifts every beat by 10 ms");
    check(throws([&] { with({{"gridOffsetMs", 1e9}}); }) && throws([&] { with({{"firstBeatMs", -5.0}}); }) && throws([&] { with({{"firstBeatMs", 999999999.0}}); }), "grid corrections stay inside sane bounds");

    auto keyed = analysis::effective_values(with({{"key", "G"}}));
    check(keyed["key"] == "G" && keyed["scale"] == "minor" && keyed["camelot"] == "6A" && keyed["keySource"] == "corrected", "key override keeps the detected scale; Camelot recalculated (G minor = 6A)");
    keyed = analysis::effective_values(with({{"scale", "major"}}));
    check(keyed["key"] == "F" && keyed["scale"] == "major" && keyed["camelot"] == "7B", "scale override: F major = 7B");
    keyed = analysis::effective_values(with({{"key", "Db"}, {"scale", "MINOR"}}));
    check(keyed["key"] == "C#" && keyed["camelot"] == "12A", "enharmonic key override normalised (Db minor -> C# minor = 12A)");
    check(throws([&] { with({{"key", "H"}}); }) && throws([&] { with({{"scale", "dorian"}}); }), "invalid key corrections rejected");
    auto keyed_doc = silent; keyed_doc["corrections"] = analysis::validate_corrections({{"key", "A"}, {"scale", "minor"}}, silent);
    check(analysis::effective_values(silent)["camelot"].is_null() && analysis::effective_values(keyed_doc)["camelot"] == "8A", "an operator key on an unkeyed track yields a Camelot code");

    // Consumers: Smart Cue snaps to the corrected grid, not the detected one.
    library::RuleBasedCueSuggestionProvider cues;
    library::CueSuggestionInput input; input.duration_ms = 155000;
    input.musical = analysis::consumer_view(with({{"bpm", {{"mode", "custom"}, {"value", 100.0}}}, {"firstBeatMs", 100.0}}));
    check(input.musical["bpm"] == 100.0 && input.musical["bpmConfidence"] == 1.0, "consumer view exposes effective BPM with full confidence");
    const auto set = cues.suggest(input, {"strong-start"});
    check(!set.suggestions.empty() && on_grid(static_cast<double>(set.suggestions[0].start_ms), 100.0, 100.0), "Smart Cue uses the corrected beat grid");

    // Store: corrections persist, survive re-analysis of the same audio, and are dropped when the audio changes.
    const auto dir = root / "corrections";
    {
        analysis::MusicalAnalysisStore store(dir);
        check(throws([&] { store.set_corrections(track_id('9'), analysis::empty_corrections()); }), "cannot correct a track that has no analysis");
        store.save(track_id('1'), sample_document(hash));
        auto updated = store.set_corrections(track_id('1'), {{"bpm", {{"mode", "factor"}, {"value", 0.5}}}, {"key", "A"}, {"scale", "minor"}});
        check(updated["effective"]["bpm"] == 64.0 && updated["effective"]["camelot"] == "8A" && updated["bpm"] == 128.0 && updated["key"] == "F", "stored corrections sit beside the untouched detection");
        const auto summary = store.summaries()[track_id('1')];
        check(summary["effectiveBpm"] == 64.0 && summary["effectiveCamelot"] == "8A" && summary["corrected"] == true && summary["bpm"] == 128.0 && summary["camelot"] == "4A", "summaries carry detected and effective values");
        store.save(track_id('1'), sample_document(hash, 127.0));
        auto again = *store.find(track_id('1'));
        check(again["bpm"] == 127.0 && again["effective"]["bpm"] == 63.5 && again["effective"]["key"] == "A", "re-analysis of the same audio keeps the corrections");
        store.save(track_id('1'), sample_document("sha256:" + std::string(64, 'c')));
        again = *store.find(track_id('1'));
        check(again["effective"]["bpmSource"] == "detected" && again["effective"]["corrected"] == false, "corrections are dropped when the audio changed");
        store.set_corrections(track_id('1'), {{"gridOffsetMs", 5.0}});
    }
    { analysis::MusicalAnalysisStore reopened(dir); check((*reopened.find(track_id('1')))["corrections"]["gridOffsetMs"] == 5.0, "corrections survive restart"); }
}

// ---- real-music validation runner (Phase 2.5) -------------------------------
class RunnerProvider final : public analysis::MusicalAnalysisProvider {
public:
    std::atomic<bool> cancelled{false};
    std::string id() const override { return "fake-mir"; }
    json capabilities() const override { return {}; }
    json musical_capabilities() const override { return {}; }
    std::string provider_version() const override { return "fake 1"; }
    void cancel_current() noexcept override { cancelled = true; }
    json execute(const analysis::AnalysisJob& job) override {
        const auto name = fs::path(job.asset_path).filename().string();
        if (name.find("bad") != std::string::npos) throw std::runtime_error("cannot decode " + name);
        if (name.find("slow") != std::string::npos) { cancelled = false; while (!cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(5)); throw std::runtime_error("analysis worker cancelled"); }
        double bpm = name.find("half") != std::string::npos ? 64.0 : name.find("off") != std::string::npos ? 131.0 : 128.0;
        auto doc = sample_document(job.content_hash, bpm);
        if (name.find("relative") != std::string::npos) { doc["key"] = "A"; doc["scale"] = "minor"; }   // 8A = relative of F? no: F minor is 4A
        if (name.find("nokey") != std::string::npos) { doc["key"] = nullptr; doc["scale"] = nullptr; }
        auto result = analysis::empty_analysis(job.track_id);
        result["musical"] = analysis::validate_musical(doc);
        return result;
    }
};

json metric_entry(double detected, json expected, const char* key = nullptr, const char* scale = nullptr, json expected_key = nullptr, double ms = 100, const char* rating = nullptr) {
    return {{"status", "done"}, {"detectedBpm", detected}, {"expectedBpm", expected}, {"detectedKeyName", key ? json(key) : json(nullptr)},
            {"detectedScale", scale ? json(scale) : json(nullptr)}, {"expectedKey", expected_key}, {"analysisDurationMs", ms}, {"smartCueRating", rating ? json(rating) : json(nullptr)}};
}

void runner_validation_tests(const fs::path& root) {
    // Key label parsing and Camelot compatibility.
    const auto parsed = [](const char* s) { const auto k = analysis::parse_key_label(s); return k ? k->key + " " + k->scale : std::string("none"); };
    check(parsed("F minor") == "F minor" && parsed("Fm") == "F minor" && parsed("F#m") == "F# minor" && parsed("Db major") == "C# major" && parsed("C") == "C major" &&
          parsed("Bbm") == "A# minor" && parsed("Bm") == "B minor" && parsed("a min") == "A minor" && parsed("Cmaj") == "C major", "expected-key labels parse (letters, accidentals, m/min/maj)");
    check(parsed("8A") == "A minor" && parsed("4a") == "F minor" && parsed("8B") == "C major" && parsed("12A") == "C# minor", "Camelot codes are accepted as expected keys");
    check(parsed("") == "none" && parsed("H minor") == "none" && parsed("C dorian") == "none" && parsed("13A") == "none", "unparseable labels are rejected");
    check(analysis::camelot_compatible("8A", "8A") && analysis::camelot_compatible("8A", "9A") && analysis::camelot_compatible("8A", "7A") && analysis::camelot_compatible("1A", "12A") &&
          analysis::camelot_compatible("8A", "8B") && !analysis::camelot_compatible("8A", "10A") && !analysis::camelot_compatible("8A", "9B") && !analysis::camelot_compatible("8A", "bogus"), "Camelot compatibility: same, +-1 on the ring (wrapping), relative major/minor");

    // Metrics: nothing invented without expectations.
    const auto none = analysis::validation_metrics(json::array({metric_entry(120, nullptr), metric_entry(90, nullptr)}));
    check(none["analysed"] == 2 && none["exactBpmAccuracy"].is_null() && none["octaveEquivalentBpmAccuracy"].is_null() && none["medianBpmError"].is_null() &&
          none["keyAccuracy"].is_null() && none["camelotCompatibleAccuracy"].is_null() && none["smartCue"]["goodRate"].is_null() && none["averageProcessingMs"] == 100.0,
          "without expectations every accuracy figure is null (only processing time is reported)");
    check(analysis::validation_metrics(json::array())["analysed"] == 0 && analysis::validation_metrics(json::array())["averageProcessingMs"].is_null(), "empty run has no figures");

    json entries = json::array({
        metric_entry(120.0, 120.0, "F", "minor", "F minor", 9000),     // exact BPM, exact key
        metric_entry(60.0, 120.0, "A", "minor", "F minor", 1000),      // half-time: octave ok; A minor 8A vs F minor 4A: not compatible
        metric_entry(131.0, 128.0, "C", "major", "A minor", 1200),     // off by 3: wrong; C major 8B vs A minor 8A: relative = compatible
        metric_entry(100.0, 100.5, "G", "minor", "D minor", 800),      // within 1 BPM: exact; 6A vs 7A adjacent = compatible
        metric_entry(90.0, nullptr, nullptr, nullptr, nullptr, 1000, "Good"),
        {{"status", "failed"}, {"filename", "x.mp3"}, {"error", "decode"}},
        metric_entry(110.0, 55.0, nullptr, nullptr, "8A", 1000, "Bad")});   // expected half of detected; no key detected -> wrong key
    const auto m = analysis::validation_metrics(entries);
    check(m["analysed"] == 6 && m["failures"] == 1 && m["failureList"][0]["filename"] == "x.mp3", "failures are counted and listed");
    check(m["bpmSamples"] == 5 && std::abs(m["exactBpmAccuracy"].get<double>() - 2.0 / 5.0) < 1e-9, "exact BPM accuracy within 1 BPM: 2/5");
    check(std::abs(m["octaveEquivalentBpmAccuracy"].get<double>() - 4.0 / 5.0) < 1e-9, "octave-equivalent accuracy accepts x2 and /2: 4/5");
    check(std::abs(m["medianBpmError"].get<double>() - 3.0) < 1e-9 && std::abs(m["medianBpmErrorOctaveAdjusted"].get<double>()) < 1e-9, "median BPM error: raw 3.0 BPM, octave-adjusted 0.0 BPM");
    check(m["keySamples"] == 5 && std::abs(m["keyAccuracy"].get<double>() - 1.0 / 5.0) < 1e-9, "exact key accuracy: 1/5 (a missed key counts as wrong)");
    check(std::abs(m["camelotCompatibleAccuracy"].get<double>() - 3.0 / 5.0) < 1e-9, "Camelot-compatible accuracy includes exact, adjacent and relative: 3/5");
    check(std::abs(m["averageProcessingMs"].get<double>() - 2333.3333333) < 1e-3 && std::abs(m["averageWarmProcessingMs"].get<double>() - 1000.0) < 1e-6, "average processing time, with and without the cold first file");
    check(m["smartCue"]["good"] == 1 && m["smartCue"]["bad"] == 1 && std::abs(m["smartCue"]["goodRate"].get<double>() - 0.5) < 1e-9, "Smart Cue ratings summarised");

    // Runner end to end with a fake provider.
    const auto music = root / "music"; fs::create_directories(music / "sub");
    const auto write = [](const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; };
    write(music / "a_plain.wav", "aaaa"); write(music / "b_half.mp3", "bbbb"); write(music / "c_bad.flac", "cccc"); write(music / "notes.txt", "ignore me");
    write(music / "sub" / "d_nokey.wav", "dddd");
    std::atomic<int> created{0};
    std::shared_ptr<RunnerProvider> provider;
    analysis::MusicValidationRunner runner(root / "validation", [&] { ++created; provider = std::make_shared<RunnerProvider>(); return provider; });
    check(throws([&] { runner.start(music / "missing", false); }) && throws([&] { runner.start(fs::path("relative"), false); }), "invalid folders are rejected");
    fs::create_directories(root / "empty");
    check(throws([&] { runner.start(root / "empty", false); }), "a folder without audio is rejected");
    const auto started = runner.start(music, false);
    check(started["total"] == 3 && started["entries"][0]["filename"] == "a_plain.wav" && started["entries"][2]["filename"] == "c_bad.flac", "non-recursive run lists only audio files, sorted");
    check(throws([&] { runner.start(music, true); }), "only one run at a time");
    const auto wait_run = [&](const std::string& id) {
        for (int i = 0; i < 1500; ++i) { auto r = runner.get(id); if (r["status"] != "queued" && r["status"] != "running") return r; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        throw std::runtime_error("validation run timed out");
    };
    const auto id = started["id"].get<std::string>();
    auto finished = wait_run(id);
    check(finished["status"] == "completed" && finished["done"] == 3 && finished["provider"]["id"] == "fake-mir", "run completes");
    const auto& e = finished["entries"];
    check(e[0]["status"] == "done" && e[0]["detectedBpm"] == 128.0 && e[0]["detectedKey"] == "F minor" && e[0]["camelot"] == "4A" && e[0]["sections"].size() == 6 && e[0]["analysisDurationMs"].is_number() &&
          e[0]["durationMs"] == 155000.0 && e[0]["smartCue"].size() >= 1, "entries carry detected values, sections, timing and Smart Cue proposals");
    check(e[1]["detectedBpm"] == 64.0 && e[2]["status"] == "failed" && e[2]["error"].get<std::string>().find("cannot decode") != std::string::npos, "one failing file does not stop the run");
    check(created == 1, "the provider is created on demand");

    // Operator expectations drive the metrics.
    check(throws([&] { runner.update_entry(id, 0, {{"expectedBpm", 5}}); }) && throws([&] { runner.update_entry(id, 0, {{"expectedKey", "H minor"}}); }) &&
          throws([&] { runner.update_entry(id, 0, {{"smartCueRating", "Perfect"}}); }) && throws([&] { runner.update_entry(id, 99, {{"notes", "x"}}); }) &&
          throws([&] { runner.update_entry(id, 0, {{"notes", std::string(501, 'x')}}); }), "expectation edits are validated");
    runner.update_entry(id, 0, {{"expectedBpm", 128.0}, {"expectedKey", "Fm"}, {"notes", "clean intro"}, {"smartCueRating", "Good"}});
    const auto updated = runner.update_entry(id, 1, {{"expectedBpm", 128.0}, {"expectedKey", "8A"}, {"smartCueRating", "Bad"}});
    check(updated["entry"]["expectedKeyNormalized"] == "A minor" && updated["metrics"]["bpmSamples"] == 2, "edited entry reports normalised key and fresh metrics");
    auto after = runner.get(id)["metrics"];
    check(std::abs(after["exactBpmAccuracy"].get<double>() - 0.5) < 1e-9 && std::abs(after["octaveEquivalentBpmAccuracy"].get<double>() - 1.0) < 1e-9 &&
          std::abs(after["keyAccuracy"].get<double>() - 0.5) < 1e-9 && after["failures"] == 1 && after["smartCue"]["good"] == 1, "run metrics follow the operator's expectations");
    runner.update_entry(id, 0, {{"expectedBpm", nullptr}});
    check(runner.get(id)["entries"][0]["expectedBpm"].is_null(), "an expectation can be cleared");

    // Exports.
    const auto exported = json::parse(runner.export_json(id));
    check(exported["entries"].size() == 3 && exported.contains("metrics"), "JSON export contains entries and metrics");
    const auto csv = runner.export_csv(id);
    check(csv.starts_with("filename,duration,detectedBpm,bpmConfidence,alternatives,detectedKey,keyConfidence,camelot,energy,sections,analysisDurationMs,errors,expectedBpm,expectedKey,notes,smartCueRating,smartCueSuggestions\n") &&
          csv.find("a_plain.wav,155,128,") != std::string::npos && csv.find("clean intro") != std::string::npos && csv.find("cannot decode c_bad.flac") != std::string::npos &&
          std::count(csv.begin(), csv.end(), '\n') == 4, "CSV export has the requested columns and one row per file");
    runner.update_entry(id, 2, {{"notes", "=HYPERLINK(\"x\"),\"quoted\""}});
    const auto safe = runner.export_csv(id);
    check(safe.find("\"'=HYPERLINK(\"\"x\"\"),\"\"quoted\"\"\"") != std::string::npos, "CSV quoting and spreadsheet-formula neutralisation");

    // Persistence: a new runner on the same directory still lists the run with its edits.
    { analysis::MusicValidationRunner reloaded(root / "validation", [] { return std::shared_ptr<analysis::MusicalAnalysisProvider>(); });
      check(reloaded.get(id)["entries"][1]["expectedKey"] == "8A" && reloaded.list().size() == 1, "runs and operator edits survive restart"); }

    // Cancellation and recursion on a second run.
    write(music / "e_slow.wav", "eeee");
    const auto second = runner.start(music, true);
    check(second["total"] == 5, "recursive run includes sub-folders");
    const auto second_id = second["id"].get<std::string>();
    for (int i = 0; i < 400; ++i) { const auto r = runner.get(second_id); if (r["entries"][3]["status"] == "pending" && r["done"].get<int>() >= 2 && provider && !provider->cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(5)); else break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(runner.cancel(second_id), "a running validation can be cancelled");
    const auto cancelled = wait_run(second_id);
    check(cancelled["status"] == "cancelled" && cancelled["entries"][3]["status"] == "pending", "cancelled run keeps finished entries and leaves the rest pending");
    check(!runner.cancel("run_unknown") && throws([&] { runner.get("../etc/passwd"); }), "unknown run IDs are rejected");

    // A provider that cannot be created fails the run cleanly.
    analysis::MusicValidationRunner broken(root / "validation-broken", []() -> std::shared_ptr<analysis::MusicalAnalysisProvider> { throw std::runtime_error("python missing"); });
    const auto bad = broken.start(music, false);
    for (int i = 0; i < 400 && broken.get(bad["id"].get<std::string>())["status"] == "queued"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int i = 0; i < 400 && broken.get(bad["id"].get<std::string>())["status"] == "running"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto failed_run = broken.get(bad["id"].get<std::string>());
    check(failed_run["status"] == "failed" && failed_run["error"].get<std::string>().find("python missing") != std::string::npos, "missing provider fails the run with a clear reason");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (sodium_init() < 0) return 1;
    const auto root = fs::current_path() / ("musical-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        camelot_tests();
        validation_tests();
        store_tests(root);
        cue_tests();
        corrections_tests(root);
        runner_validation_tests(root);
        scheduler_tests(root);
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what()); ++failures;
    }
    std::error_code ignored; fs::remove_all(root, ignored);
    std::printf("musical failures: %d\n", failures);
    return failures ? 1 : 0;
}
