#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
namespace mapping_probe {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
inline Json stages = Json::object(), stats = Json::object();
inline Clock::time_point previous;
inline void start() { previous = Clock::now(); }
inline double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
}
inline void mark(const char* name) {
    const auto now = Clock::now();
    stages[name] = std::chrono::duration<double, std::milli>(now-previous).count();
    previous = now;
}
}
