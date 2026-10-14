#include "e2e_trace.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#ifdef OBJ_PROFILE_NVTX
#include <nvtx3/nvToolsExt.h>
#endif
#ifdef OBJ_PROFILE_ITT
#include <ittnotify.h>
#endif

namespace obj_profile {
namespace {
const auto epoch = Clock::now();
std::atomic<uint64_t> nextId = 1;
thread_local std::vector<uint64_t> parents;
std::mutex outputMutex;
FILE* output = nullptr;
bool opened = false;
uint64_t ticks(const FILETIME& value) {
    return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
double milliseconds(Clock::time_point point) {
    return std::chrono::duration<double, std::milli>(point - epoch).count();
}
void write(Json value) {
    std::lock_guard lock(outputMutex);
    if (!opened) {
        opened = true;
        wchar_t path[32768]{}; size_t required = 0;
        if (!_wgetenv_s(&required, path, std::size(path), L"WOBY_E2E_TRACE_PATH") && required > 1) {
            (void)_wfopen_s(&output, path, L"wb");
        }
        if (output) {
            LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
            const Json metadata{{"kind", "metadata"}, {"pid", GetCurrentProcessId()},
                {"steady_epoch_nanoseconds", std::chrono::duration_cast<std::chrono::nanoseconds>(epoch.time_since_epoch()).count()},
                {"qpc_frequency", frequency.QuadPart},
                {"method", "Thread-local nesting; coarse phase counters; process counters overlap across threads"}};
            const auto line = metadata.dump() + '\n';
            (void)std::fwrite(line.data(), 1, line.size(), output);
        }
    }
    if (!output) { return; }
    value["thread_id"] = GetCurrentThreadId();
    const auto line = value.dump() + '\n';
    (void)std::fwrite(line.data(), 1, line.size(), output);
    (void)std::fflush(output);
}
}
uint64_t threadCpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    return ticks(kernel) + ticks(user);
}
Counters counters() {
    Counters result;
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    result.user = ticks(user); result.kernel = ticks(kernel); result.thread = threadCpu();
    ULONG64 cycles = 0; QueryProcessCycleTime(GetCurrentProcess(), &cycles); result.cycles = cycles;
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
        result.reads = io.ReadTransferCount; result.writes = io.WriteTransferCount; result.readOps = io.ReadOperationCount;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        result.workingSet = memory.WorkingSetSize; result.commit = memory.PrivateUsage;
        result.peakWorkingSet = memory.PeakWorkingSetSize; result.peakCommit = memory.PeakPagefileUsage;
        result.faults = memory.PageFaultCount;
    }
    return result;
}
uint64_t traceBegin(const char* name) {
#ifdef OBJ_PROFILE_ITT
    static const auto domain = __itt_domain_create("woby.end_to_end");
    __itt_task_begin(domain, __itt_null, __itt_null, __itt_string_handle_create(name));
#endif
#ifdef OBJ_PROFILE_NVTX
    return nvtxRangeStartA(name);
#else
    (void)name; return 0;
#endif
}
void traceEnd(uint64_t id) {
#ifdef OBJ_PROFILE_ITT
    static const auto domain = __itt_domain_create("woby.end_to_end");
    __itt_task_end(domain);
#endif
#ifdef OBJ_PROFILE_NVTX
    nvtxRangeEnd(id);
#else
    (void)id;
#endif
}
Phase begin(const char* name) {
    Phase phase{name, {}, {}, nextId.fetch_add(1), parents.empty() ? 0 : parents.back(), traceBegin(name)};
    phase.before = counters(); phase.start = Clock::now(); parents.push_back(phase.id);
    return phase;
}
void end(const Phase& phase, Json data) {
    const auto finish = Clock::now(); const auto after = counters(); traceEnd(phase.trace);
    const bool nesting = !parents.empty() && parents.back() == phase.id;
    if (nesting) { parents.pop_back(); }
    const auto elapsed = std::chrono::duration<double, std::milli>(finish - phase.start).count();
    const auto cpu = static_cast<double>(after.user + after.kernel - phase.before.user - phase.before.kernel) / 10000.0;
    write({{"kind", "phase"}, {"name", phase.name}, {"id", phase.id}, {"parent_id", phase.parent},
        {"nesting_valid", nesting}, {"start_ms", milliseconds(phase.start)}, {"end_ms", milliseconds(finish)},
        {"wall_ms", elapsed}, {"cpu_ms", cpu}, {"average_logical_cpus", elapsed > 0 ? cpu / elapsed : 0},
        {"user_ms", static_cast<double>(after.user - phase.before.user) / 10000.0},
        {"kernel_ms", static_cast<double>(after.kernel - phase.before.kernel) / 10000.0},
        {"caller_cpu_ms", static_cast<double>(after.thread - phase.before.thread) / 10000.0},
        {"cycles", after.cycles - phase.before.cycles}, {"read_bytes", after.reads - phase.before.reads},
        {"write_bytes", after.writes - phase.before.writes}, {"read_ops", after.readOps - phase.before.readOps},
        {"page_faults", after.faults - phase.before.faults},
        {"working_set_begin", phase.before.workingSet}, {"working_set_end", after.workingSet},
        {"commit_begin", phase.before.commit}, {"commit_end", after.commit},
        {"process_peak_working_set", after.peakWorkingSet}, {"process_peak_commit", after.peakCommit}, {"data", std::move(data)}});
}
void next(Phase& phase, const char* name, Json data) { end(phase, std::move(data)); phase = begin(name); }
void mark(const char* name, Json data) {
    write({{"kind", "mark"}, {"name", name}, {"time_ms", milliseconds(Clock::now())}, {"data", std::move(data)}});
}
void worker(double elapsedMs, double ioMs, uint64_t beforeCpu) {
    const auto finish = milliseconds(Clock::now());
    write({{"kind", "worker"}, {"name", "parser_worker_read_decode"}, {"start_ms", finish - elapsedMs},
        {"end_ms", finish}, {"wall_ms", elapsedMs}, {"io_submit_wait_ms", ioMs},
        {"caller_cpu_ms", static_cast<double>(threadCpu() - beforeCpu) / 10000.0}});
}
size_t reserveEstimate(size_t current, size_t maximum) { return std::min(current, maximum); }
} // namespace obj_profile
