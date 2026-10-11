#include "liveplay/library/media_service.hpp"
#include "liveplay/util/atomic_json.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace liveplay::library {
namespace fs = std::filesystem;
namespace {
std::string quote(const std::string& value) {
#if defined(_WIN32)
    std::string out = "\""; for (char c : value) { if (c == '\"') out += '\\'; out += c; } return out + "\"";
#else
    return "'" + value + "'";
#endif
}
std::string capture(const std::string& command) {
    std::array<char, 4096> buffer{}; std::string output;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE read_pipe{}, write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) throw std::runtime_error("cannot create media worker pipe");
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = write_pipe; si.hStdError = write_pipe;
    PROCESS_INFORMATION pi{}; std::wstring line(command.begin(), command.end());
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(read_pipe); CloseHandle(write_pipe); throw std::runtime_error("cannot start media worker"); }
    CloseHandle(write_pipe); DWORD count{};
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count) output.append(buffer.data(), count);
    WaitForSingleObject(pi.hProcess, 30000); DWORD code{}; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(read_pipe); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code != 0) throw std::runtime_error("media worker failed: " + output.substr(0, 512));
    return output;
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("cannot start media worker");
    while (const auto n = std::fread(buffer.data(), 1, buffer.size(), pipe)) output.append(buffer.data(), n);
    const int code = pclose(pipe);
    if (code != 0) throw std::runtime_error("media worker failed: " + output.substr(0, 512));
    return output;
#endif
}
double rate(const std::string& text) {
    const auto slash = text.find('/');
    try { return slash == std::string::npos ? std::stod(text) : std::stod(text.substr(0, slash)) / std::stod(text.substr(slash + 1)); } catch (...) { return 0; }
}
}

TrackMedia probe_media_file(const fs::path& source) {
    const char* configured = std::getenv("LIVEPLAY_FFPROBE");
    const auto ffprobe = configured && *configured ? configured : "ffprobe";
    const auto doc = nlohmann::json::parse(capture(quote(ffprobe) + " -v error -show_format -show_streams -of json " + quote(util::path_to_utf8(source))));
    TrackMedia out; out.has_audio = false; out.has_video = false; out.container = doc.value("format", nlohmann::json::object()).value("format_name", "");
    double duration = 0; try { duration = std::stod(doc.value("format", nlohmann::json::object()).value("duration", "0")); } catch (...) {}
    for (const auto& stream : doc.value("streams", nlohmann::json::array())) {
        const auto type = stream.value("codec_type", "");
        if (type == "audio") { out.has_audio = true; if (out.audio_codec.empty()) out.audio_codec = stream.value("codec_name", ""); if (!out.sample_rate) { try { out.sample_rate = static_cast<std::uint32_t>(std::stoul(stream.value("sample_rate", "0"))); } catch (...) {} } if (!out.audio_channels) out.audio_channels = stream.value("channels", 0u); }
        if (type == "video") { out.has_video = true; if (out.video_codec.empty()) out.video_codec = stream.value("codec_name", ""); out.width = std::max(out.width, stream.value("width", 0u)); out.height = std::max(out.height, stream.value("height", 0u)); if (out.frame_rate == 0) out.frame_rate = rate(stream.value("avg_frame_rate", "0/1")); }
    }
    out.media_type = out.has_video ? "video" : "audio";
    out.probe_status = "ready";
    if (!out.has_audio && !out.has_video) throw std::runtime_error("media has no playable streams");
    out.duration_ms = static_cast<std::int64_t>(duration * 1000.0);
    return out;
}

AudioProxyService::AudioProxyService(fs::path root, std::function<bool()> low_impact, std::function<void(Track)> on_ready)
    : root_(std::move(root)), low_impact_(std::move(low_impact)), on_ready_(std::move(on_ready)), worker_([this] { run(); }) { fs::create_directories(root_); }
AudioProxyService::~AudioProxyService() { { std::lock_guard lock(mutex_); stopping_ = true; } cv_.notify_all(); if (worker_.joinable()) worker_.join(); }
fs::path AudioProxyService::proxy_path(const Track& track) const { return root_ / (track.content_hash.substr(7) + "-pcm-s16le-48000-stereo.wav"); }
void AudioProxyService::request(const Track& track) { std::lock_guard lock(mutex_); const auto path = proxy_path(track); if (fs::is_regular_file(path)) { states_[track.id] = "ready"; return; } if (states_[track.id] == "queued" || states_[track.id] == "running") return; states_[track.id] = "queued"; errors_.erase(track.id); queue_.push_back(track); cv_.notify_one(); }
nlohmann::json AudioProxyService::status(const Track& track) const { std::lock_guard lock(mutex_); const auto path = proxy_path(track); const auto state = fs::is_regular_file(path) ? "ready" : (states_.contains(track.id) ? states_.at(track.id) : "absent"); return {{"track_id",track.id},{"status",state},{"path",state == "ready" ? util::path_to_utf8(path) : ""},{"cache_key",track.content_hash + ":pcm-s16le:48000:2"},{"error",errors_.contains(track.id) ? errors_.at(track.id) : ""}}; }
bool AudioProxyService::busy() const { std::lock_guard lock(mutex_); return !queue_.empty() || std::any_of(states_.begin(), states_.end(), [](const auto& item) { return item.second == "running"; }); }
nlohmann::json AudioProxyService::cache_status() const { std::uintmax_t bytes = 0, files = 0; std::error_code ec; if (fs::exists(root_, ec)) for (fs::recursive_directory_iterator it(root_, ec), end; it != end && !ec; it.increment(ec)) if (it->is_regular_file(ec)) { bytes += it->file_size(ec); ++files; } return {{"path", util::path_to_utf8(root_)}, {"bytes", bytes}, {"files", files}, {"busy", busy()}}; }
bool AudioProxyService::clear_regenerable_cache(std::string& error) { if (busy()) { error = "audio proxy work is active"; return false; } std::error_code ec; fs::remove_all(root_, ec); if (ec) { error = ec.message(); return false; } fs::create_directories(root_, ec); if (ec) { error = ec.message(); return false; } return true; }
void AudioProxyService::run() { while (true) { Track track; { std::unique_lock lock(mutex_); cv_.wait(lock,[this]{return stopping_ || !queue_.empty();}); if(stopping_) return; track=queue_.front(); queue_.pop_front(); states_[track.id]="running"; } try { while (low_impact_ && low_impact_()) std::this_thread::sleep_for(std::chrono::milliseconds(250)); const auto target = proxy_path(track); const auto temp = fs::path(target.string()+".partial.wav"); const char* configured=std::getenv("LIVEPLAY_FFMPEG"); const auto ffmpeg=configured&&*configured?configured:"ffmpeg"; const auto output=quote(util::path_to_utf8(temp)); const auto command = track.media.has_audio ? quote(ffmpeg)+" -y -v error -i "+quote(track.asset_path)+" -map 0:a:0 -vn -ac 2 -ar 48000 -c:a pcm_s16le "+output : quote(ffmpeg)+" -y -v error -f lavfi -i anullsrc=r=48000:cl=stereo -t "+std::to_string(std::max<std::int64_t>(1, track.duration_ms) / 1000.0)+" -ac 2 -ar 48000 -c:a pcm_s16le "+output; capture(command); std::error_code ec; fs::remove(target, ec); fs::rename(temp,target); track.media.audio_proxy_path=util::path_to_utf8(target); track.media.audio_proxy_key=track.content_hash+":pcm-s16le:48000:2"; if(on_ready_) on_ready_(track); { std::lock_guard lock(mutex_); states_[track.id]="ready"; } } catch(const std::exception& e) { std::lock_guard lock(mutex_); states_[track.id]="failed"; errors_[track.id]=e.what(); } } }
} // namespace liveplay::library
