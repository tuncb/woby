#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <string>

namespace woby { struct Mesh; }

// This API is used only by generated research copies of the loader.
namespace obj_profile {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
struct Counters {
    uint64_t user = 0, kernel = 0, thread = 0, cycles = 0;
    uint64_t reads = 0, writes = 0, readOps = 0, faults = 0;
    uint64_t workingSet = 0, commit = 0, peakWorkingSet = 0, peakCommit = 0;
};
struct Phase {
    std::string name;
    Clock::time_point start;
    Counters before;
    unsigned previous = 0;
    uint64_t trace = 0;
};
Counters counters();
Phase begin(const char* name);
void end(const Phase& phase, Json data = Json::object());
void next(Phase& phase, const char* name, Json data = Json::object());
void worker(double elapsedMs, double ioMs, uint64_t beforeCpu);
uint64_t threadCpu();
Json result();
Json cpuCapabilities();
Json clockOrigin();
uint64_t traceBegin(const char* name);
void traceEnd(uint64_t id);
size_t reserveEstimate(size_t current, size_t maximum);
Json deepReplay(const woby::Mesh& mesh);
Json simdReplay(const woby::Mesh& mesh);
void startSampling(bool instructions);
Json stopSampling();
} // namespace obj_profile
