// Standalone JSON-Lines worker used by FFmpegBasicAnalysisProvider. Kept out
// of the server process so decode/filter failures cannot affect audio control.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
std::string quote(const std::string& value) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : value) { if (c == '\"') out += '\\'; out += c; }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : value) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
#endif
}
std::string capture(const std::string& command) {
    std::string output; std::array<char, 4096> buffer{};
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) throw std::runtime_error("cannot start ffmpeg");
    while (const auto n = std::fread(buffer.data(), 1, buffer.size(), pipe)) output.append(buffer.data(), n);
#if defined(_WIN32)
    const int code = _pclose(pipe);
#else
    const int code = pclose(pipe);
#endif
    if (code != 0) throw std::runtime_error("ffmpeg failed");
    return output;
}
std::optional<double> last_number(const std::string& text, const std::regex& expression) {
    std::optional<double> result;
    for (std::sregex_iterator it(text.begin(), text.end(), expression), end; it != end; ++it) {
        try { result = std::stod((*it)[1].str()); } catch (...) {}
    }
    return result;
}
}
int main() {
    std::string line; if (!std::getline(std::cin, line)) return 2;
    nlohmann::json request;
    try { request = nlohmann::json::parse(line); } catch (...) { return 3; }
    const auto id = request.value("requestId", "");
    if (request.value("protocolVersion", 0) != 1 || id.empty()) return 4;
    if (request.value("type", "") == "health") {
        try {
            const char* configured = std::getenv("LIVEPLAY_FFMPEG");
            capture(quote(configured && *configured ? configured : "ffmpeg") + " -version 2>&1");
            std::cout << nlohmann::json{{"protocolVersion",1},{"type","health"},{"requestId",id},{"status","ok"}}.dump() << '\n';
        } catch (const std::exception& e) {
            std::cout << nlohmann::json{{"protocolVersion",1},{"type","health"},{"requestId",id},{"status","error"},{"error",e.what()}}.dump() << '\n';
        }
        return 0;
    }
    if (request.value("type", "") != "analyze") return 5;
    try {
        const auto path = request.at("path").get<std::string>();
        const char* configured = std::getenv("LIVEPLAY_FFMPEG");
        const auto ffmpeg = quote(configured && *configured ? configured : "ffmpeg");
        // ebur128 is authoritative for LUFS/true peak; astats adds useful
        // peak/RMS when the installed FFmpeg exposes it. Both are offline.
        const auto output = capture(ffmpeg + " -hide_banner -nostats -i " + quote(path) +
            " -af \"ebur128=peak=true,astats=metadata=0:reset=0\" -f null - 2>&1");
        const std::regex duration_re("Duration: ([0-9]+):([0-9]+):([0-9]+(?:\\.[0-9]+)?)");
        std::smatch duration_match; double duration_ms = 0;
        if (std::regex_search(output, duration_match, duration_re)) {
            duration_ms = (std::stod(duration_match[1]) * 3600 + std::stod(duration_match[2]) * 60 + std::stod(duration_match[3])) * 1000;
        }
        const auto lufs = last_number(output, std::regex("(?:^|\\n)\\s*I:\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*LUFS"));
        const auto true_peak = last_number(output, std::regex("(?:True peak:|Peak:)\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*dB"));
        const auto rms = last_number(output, std::regex("RMS level dB:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"));
        const auto peak = last_number(output, std::regex("Peak level dB:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"));
        nlohmann::json basic{{"duration_ms", duration_ms > 0 ? nlohmann::json(duration_ms) : nlohmann::json(nullptr)},
            {"peak_dbfs", peak ? nlohmann::json(*peak) : nlohmann::json(nullptr)},
            {"rms_dbfs", rms ? nlohmann::json(*rms) : nlohmann::json(nullptr)}};
        nlohmann::json loudness{{"integrated_lufs", lufs ? nlohmann::json(*lufs) : nlohmann::json(nullptr)},
            {"true_peak_dbtp", true_peak ? nlohmann::json(*true_peak) : nlohmann::json(nullptr)}, {"lra", nullptr}};
        nlohmann::json data{{"basic", basic}, {"loudness", loudness}};
        if (lufs) data["recommended_gain_db"] = std::clamp(-12.0 - *lufs, -12.0, 6.0);
        else data["recommended_gain_db"] = nullptr;
        std::cout << nlohmann::json{{"protocolVersion",1},{"type","result"},{"requestId",id},{"status","ok"},{"data",data}}.dump() << '\n';
    } catch (const std::exception& e) {
        std::cout << nlohmann::json{{"protocolVersion",1},{"type","result"},{"requestId",id},{"status","error"},{"error",e.what()}}.dump() << '\n';
    }
    return 0;
}
