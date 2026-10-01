#include "liveplay/analysis/worker_adapter.hpp"
#include "liveplay/util/unicode_path.hpp"
#include <future>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace liveplay::analysis {
ExternalAnalysisProvider::ExternalAnalysisProvider(std::filesystem::path executable, std::chrono::milliseconds timeout,
                                                   std::string provider_id, json capabilities)
    : executable_(std::move(executable)), timeout_(timeout), provider_id_(std::move(provider_id)), capabilities_(std::move(capabilities)) {
    if (executable_.empty()) throw std::invalid_argument("analysis worker executable is empty");
    if (provider_id_.empty()) throw std::invalid_argument("analysis provider ID is empty");
}
ExternalAnalysisProvider::~ExternalAnalysisProvider() { cancel(); }
json ExternalAnalysisProvider::capabilities() const {
    json result = {{"basic",false},{"loudness",false},{"rhythm",false},{"tonality",false},{"structure",false},{"semantic",false},{"stems",false}};
    for (const auto& [key, value] : capabilities_.items()) if (result.contains(key) && value.is_boolean()) result[key] = value;
    return result;
}
void ExternalAnalysisProvider::cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_release);
    std::lock_guard lock(process_mutex_);
#if defined(_WIN32)
    if (active_process_) TerminateProcess(static_cast<HANDLE>(active_process_), 2);
#else
    if (active_pid_ > 0) ::kill(active_pid_, SIGKILL);
#endif
}
json ExternalAnalysisProvider::exchange(const json& request) {
    cancel_requested_.store(false, std::memory_order_release);
    std::string input = request.dump() + "\n", output;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; HANDLE child_in_read{}, child_in_write{}, child_out_read{}, child_out_write{};
    if (!CreatePipe(&child_in_read, &child_in_write, &sa, 0) || !CreatePipe(&child_out_read, &child_out_write, &sa, 0)) throw std::runtime_error("cannot create analysis worker pipes");
    SetHandleInformation(child_in_write, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(child_out_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = child_in_read; si.hStdOutput = child_out_write; si.hStdError = child_out_write;
    PROCESS_INFORMATION pi{}; std::wstring command = L"\"" + executable_.wstring() + L"\"";
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(child_in_read); CloseHandle(child_out_write);
    if (!created) { CloseHandle(child_in_write); CloseHandle(child_out_read); throw std::runtime_error("analysis worker could not be started"); }
    { std::lock_guard lock(process_mutex_); active_process_ = pi.hProcess; }
    DWORD written{}; const BOOL sent = WriteFile(child_in_write, input.data(), static_cast<DWORD>(input.size()), &written, nullptr); CloseHandle(child_in_write);
    if (!sent || written != input.size()) { TerminateProcess(pi.hProcess, 3); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(child_out_read); throw std::runtime_error("analysis worker input failed"); }
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    bool timed_out = false;
    while (true) {
        DWORD available{}; if (PeekNamedPipe(child_out_read, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[4096]; DWORD read{}; if (ReadFile(child_out_read, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr)) output.append(buffer, read);
            if (output.size() > 8ull * 1024 * 1024) { TerminateProcess(pi.hProcess, 4); break; }
        }
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;
        if (cancel_requested_.load(std::memory_order_acquire)) break;
        if (std::chrono::steady_clock::now() >= deadline) { timed_out = true; TerminateProcess(pi.hProcess, 5); break; }
    }
    WaitForSingleObject(pi.hProcess, 1000);
    while (true) { char buffer[4096]; DWORD read{}; if (!ReadFile(child_out_read, buffer, sizeof(buffer), &read, nullptr) || !read) break; output.append(buffer, read); }
    DWORD exit_code{}; GetExitCodeProcess(pi.hProcess, &exit_code);
    { std::lock_guard lock(process_mutex_); active_process_ = nullptr; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(child_out_read);
    if (timed_out) throw std::runtime_error("analysis worker timeout");
    if (cancel_requested_.load(std::memory_order_acquire)) throw std::runtime_error("analysis worker cancelled");
    if (exit_code != 0) throw std::runtime_error("analysis worker terminated unexpectedly");
#else
    int in_pipe[2], out_pipe[2]; if (pipe(in_pipe) || pipe(out_pipe)) throw std::runtime_error("cannot create analysis worker pipes");
    const pid_t pid = fork();
    if (pid == 0) { dup2(in_pipe[0], STDIN_FILENO); dup2(out_pipe[1], STDOUT_FILENO); close(in_pipe[1]); close(out_pipe[0]); execl(executable_.c_str(), executable_.c_str(), nullptr); _exit(127); }
    close(in_pipe[0]); close(out_pipe[1]); { std::lock_guard lock(process_mutex_); active_pid_ = pid; }
    if (write(in_pipe[1], input.data(), input.size()) != static_cast<ssize_t>(input.size())) { kill(pid, SIGKILL); }
    close(in_pipe[1]);
    auto reader = std::async(std::launch::async, [&] { char buffer[4096]; ssize_t count; while ((count = read(out_pipe[0], buffer, sizeof(buffer))) > 0) { output.append(buffer, count); if (output.size() > 8ull*1024*1024) break; } });
    const auto deadline = std::chrono::steady_clock::now() + timeout_; int status{};
    while (waitpid(pid, &status, WNOHANG) == 0) { if (cancel_requested_ || std::chrono::steady_clock::now() >= deadline) { kill(pid, SIGKILL); waitpid(pid,&status,0); break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    close(out_pipe[0]); reader.wait(); { std::lock_guard lock(process_mutex_); active_pid_ = -1; }
    if (cancel_requested_) throw std::runtime_error("analysis worker cancelled");
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("analysis worker timeout");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("analysis worker terminated unexpectedly");
#endif
    const auto newline = output.find('\n'); if (newline != std::string::npos) output.resize(newline);
    try { return json::parse(output); } catch (...) { throw std::runtime_error("analysis worker returned malformed JSON"); }
}
bool ExternalAnalysisProvider::health_check() {
    const auto request_id = "health-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto reply = exchange({{"protocolVersion",1},{"type","health"},{"requestId",request_id}});
    return reply.value("protocolVersion",0) == 1 && reply.value("type","") == "health" && reply.value("requestId","") == request_id && reply.value("status","") == "ok";
}
json ExternalAnalysisProvider::execute(const AnalysisJob& job) {
    const auto request_id = job.id + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto reply = exchange({{"protocolVersion",1},{"type","analyze"},{"requestId",request_id},{"trackId",job.track_id},{"path",job.asset_path},{"contentHash",job.content_hash}});
    if (reply.value("protocolVersion",0) != 1 || reply.value("type","") != "result" || reply.value("requestId","") != request_id || reply.value("status","") != "ok" || !reply.value("data",json::object()).is_object())
        throw std::runtime_error("analysis worker protocol mismatch");
    auto result = empty_analysis(job.track_id); result["status"] = "completed";
    for (const auto& [key,value] : reply.at("data").items()) result[key] = value;
    return result;
}
}
