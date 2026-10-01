#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include "liveplay/library/store_lock.hpp"
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--lock-probe") {
        try { liveplay::library::StoreLock lock{std::filesystem::path(argv[2])}; return 0; }
        catch (...) { return 42; }
    }
    std::string line; if (!std::getline(std::cin, line)) return 2;
    nlohmann::json request; try { request = nlohmann::json::parse(line); } catch (...) { return 3; }
    const auto path = request.value("path", "");
    if (path.find("worker-crash") != std::string::npos) return 9;
    if (path.find("worker-timeout") != std::string::npos) std::this_thread::sleep_for(std::chrono::seconds(5));
    if (path.find("worker-malformed") != std::string::npos) { std::cout << "not-json\n"; return 0; }
    if (request.value("type", "") == "health")
        std::cout << nlohmann::json{{"protocolVersion",1},{"type","health"},{"requestId",request.value("requestId","")},{"status","ok"}}.dump() << '\n';
    else
        std::cout << nlohmann::json{{"protocolVersion",1},{"type","result"},{"requestId",request.value("requestId","")},{"status","ok"},{"data",nlohmann::json::object()}}.dump() << '\n';
    return 0;
}
