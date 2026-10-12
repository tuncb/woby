#include "profile.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <cstdlib>
#include <cwchar>
#include <thread>
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
std::vector<std::string> names{"idle"};
Json events = Json::array(), workers = Json::array();
std::mutex workerMutex;
std::atomic<unsigned> active = 0;
struct Observation { unsigned phase; double time; Counters counts; };
struct Address { unsigned phase; uint64_t ip; uint64_t count; };
std::vector<Observation> observations;
std::map<std::pair<unsigned, uint64_t>, uint64_t> addresses;
std::jthread sampler;
uint64_t samplingCpu = 0, suspendFailures = 0;
uint64_t inventories = 0;
double inventoryMs = 0, contextMs = 0;
double ms(Clock::time_point start, Clock::time_point finish) {
    return std::chrono::duration<double, std::milli>(finish - start).count();
}
uint64_t ticks(const FILETIME& time) {
    return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
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
    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    result.cycles = cycles;
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
    static const auto domain = __itt_domain_create("woby.obj_load");
    __itt_task_begin(domain, __itt_null, __itt_null, __itt_string_handle_create(name));
#endif
#ifdef OBJ_PROFILE_NVTX
    return nvtxRangeStartA(name);
#else
    (void)name;
    return 0;
#endif
}
void traceEnd(uint64_t id) {
#ifdef OBJ_PROFILE_ITT
    static const auto domain = __itt_domain_create("woby.obj_load");
    __itt_task_end(domain);
#endif
#ifdef OBJ_PROFILE_NVTX
    nvtxRangeEnd(id);
#else
    (void)id;
#endif
}
Phase begin(const char* name) {
    const auto found = std::find(names.begin(), names.end(), name);
    unsigned id = static_cast<unsigned>(found - names.begin());
    if (found == names.end()) { names.emplace_back(name); }
    Phase result{name, {}, {}, active.exchange(id)};
    result.trace = traceBegin(name);
    result.before = counters();
    result.start = Clock::now();
    return result;
}
void end(const Phase& phase, Json data) {
    const auto finish = Clock::now();
    traceEnd(phase.trace);
    const auto after = counters();
    active.store(phase.previous);
    const auto& before = phase.before;
    const auto elapsed = ms(phase.start, finish);
    const double cpuMs = static_cast<double>(after.user + after.kernel - before.user - before.kernel) / 10000.0;
    events.push_back({{"name", phase.name}, {"parent", names[phase.previous]},
        {"start_ms", ms(epoch, phase.start)}, {"end_ms", ms(epoch, finish)}, {"wall_ms", elapsed},
        {"cpu_ms", cpuMs}, {"user_ms", static_cast<double>(after.user - before.user) / 10000.0},
        {"kernel_ms", static_cast<double>(after.kernel - before.kernel) / 10000.0},
        {"caller_cpu_ms", static_cast<double>(after.thread - before.thread) / 10000.0},
        {"average_logical_cpus", elapsed > 0 ? cpuMs / elapsed : 0},
        {"cycles", after.cycles - before.cycles}, {"read_bytes", after.reads - before.reads},
        {"write_bytes", after.writes - before.writes}, {"read_ops", after.readOps - before.readOps},
        {"page_faults", after.faults - before.faults},
        {"working_set_begin", before.workingSet}, {"working_set_end", after.workingSet},
        {"commit_begin", before.commit}, {"commit_end", after.commit},
        {"process_peak_working_set", after.peakWorkingSet}, {"process_peak_commit", after.peakCommit},
        {"data", std::move(data)}});
}
void next(Phase& phase, const char* name, Json data) { end(phase, std::move(data)); phase = begin(name); }
void worker(double elapsedMs, double ioMs, uint64_t beforeCpu) {
    const auto cpuMs = static_cast<double>(threadCpu() - beforeCpu) / 10000.0;
    const auto finishMs = ms(epoch, Clock::now());
    std::lock_guard lock(workerMutex);
    workers.push_back({{"thread_id", GetCurrentThreadId()}, {"wall_ms", elapsedMs}, {"io_submit_wait_ms", ioMs}, {"cpu_ms", cpuMs},
        {"start_ms", finishMs - elapsedMs}, {"end_ms", finishMs}});
}
Json result() { return {{"events", events}, {"parser_workers", workers}}; }
Json clockOrigin() {
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    return {{"steady_epoch_nanoseconds", std::chrono::duration_cast<std::chrono::nanoseconds>(epoch.time_since_epoch()).count()},
        {"qpc_frequency", frequency.QuadPart}, {"pid", GetCurrentProcessId()}, {"main_thread_id", GetCurrentThreadId()}};
}
size_t reserveEstimate(size_t current, size_t maximum) {
    wchar_t value[32]{}; size_t needed = 0;
    if (_wgetenv_s(&needed, value, 32, L"WOBY_PROFILE_RESERVE_PERCENT") || needed <= 1) { return current; }
    size_t used = 0;
    const auto percent = std::stoull(value, &used);
    if (used != std::wcslen(value) || percent > 100) { throw std::invalid_argument("Reserve percentage must be 0..100"); }
    return std::min(maximum, current + static_cast<size_t>((current * percent + 99) / 100));
}
Json cpuCapabilities() {
    std::array<int, 4> leaf{};
    __cpuid(leaf.data(), 1);
    const auto ecx = static_cast<unsigned>(leaf[2]);
    const bool osxsave = (ecx & (1u << 27)) != 0;
    const auto xcr0 = osxsave ? _xgetbv(0) : 0;
    const bool avxOs = (xcr0 & 6) == 6;
    const bool avx = (ecx & (1u << 28)) != 0;
    __cpuidex(leaf.data(), 7, 0);
    const auto ebx = static_cast<unsigned>(leaf[1]);
    char brand[49]{};
    for (int i = 0; i < 3; ++i) {
        __cpuid(leaf.data(), static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
        std::memcpy(brand + i * 16, leaf.data(), 16);
    }
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    return {{"brand", brand}, {"logical_processors", system.dwNumberOfProcessors}, {"xcr0", xcr0},
        {"sse2", true}, {"sse42", (ecx & (1u << 20)) != 0}, {"avx", avx}, {"avx_os_enabled", avxOs},
        {"avx2", (ebx & (1u << 5)) != 0 && avxOs}, {"fma", (ecx & (1u << 12)) != 0 && avxOs},
        {"avx512f", (ebx & (1u << 16)) != 0}, {"bmi1", (ebx & (1u << 3)) != 0}, {"bmi2", (ebx & (1u << 8)) != 0}};
}
void startSampling(bool instructions) {
    sampler = std::jthread([instructions](std::stop_token stop) {
        const auto ownId = GetCurrentThreadId();
        const auto cpuStart = threadCpu();
        struct Thread { DWORD id; HANDLE handle; uint64_t lastCpu; };
        std::vector<Thread> threads;
        std::map<DWORD, uint64_t> previousCpu;
        size_t nextThread = 0;
        auto lastInventory = Clock::now() - std::chrono::seconds(1);
        auto lastResources = lastInventory;
        while (!stop.stop_requested()) {
            const auto now = Clock::now();
            if (instructions && now - lastInventory >= std::chrono::milliseconds(2000)) {
                const auto inventoryStart = Clock::now();
                for (const auto& thread : threads) { CloseHandle(thread.handle); }
                threads.clear();
                const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
                if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
                    do {
                        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == ownId) { continue; }
                        const auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                        if (handle) { threads.push_back({entry.th32ThreadID, handle, previousCpu[entry.th32ThreadID]}); }
                    } while (Thread32Next(snapshot, &entry));
                }
                if (snapshot != INVALID_HANDLE_VALUE) { CloseHandle(snapshot); }
                lastInventory = Clock::now();
                inventoryMs += ms(inventoryStart, lastInventory);
                ++inventories;
            }
            const auto phase = active.load();
            if (now - lastResources >= std::chrono::milliseconds(20)) {
                observations.push_back({phase, ms(epoch, now), counters()});
                lastResources = now;
            }
            if (instructions) {
                for (size_t i = 0; i < threads.size(); ++i) {
                    auto& thread = threads[nextThread++ % threads.size()];
                    FILETIME created{}, exited{}, kernel{}, user{};
                    if (!GetThreadTimes(thread.handle, &created, &exited, &kernel, &user)) { continue; }
                    const auto cpu = ticks(user) + ticks(kernel);
                    // Do not count threads which have not consumed CPU since the last poll.
                    if (cpu == thread.lastCpu) { continue; }
                    thread.lastCpu = cpu;
                    previousCpu[thread.id] = cpu;
                    CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
                    const auto contextStart = Clock::now();
                    if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) { ++suspendFailures; continue; }
                    const bool got = GetThreadContext(thread.handle, &context) != FALSE;
                    ResumeThread(thread.handle); // Resume before any allocation or symbol work.
                    contextMs += ms(contextStart, Clock::now());
                    if (got) { ++addresses[{phase, context.Rip}]; }
                    else { ++suspendFailures; }
                    break; // Bound context-capture work to one active thread per poll.
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (const auto& thread : threads) { CloseHandle(thread.handle); }
        samplingCpu = threadCpu() - cpuStart;
    });
}
Json stopSampling() {
    sampler.request_stop(); sampler.join();
    Json samples = Json::array(), ips = Json::array();
    for (const auto& sample : observations) {
        samples.push_back({{"phase", names[sample.phase]}, {"time_ms", sample.time},
            {"cpu_ms", static_cast<double>(sample.counts.user + sample.counts.kernel) / 10000.0},
            {"working_set", sample.counts.workingSet}, {"commit", sample.counts.commit}});
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const auto process = GetCurrentProcess();
    const bool symbols = !addresses.empty() && SymInitialize(process, nullptr, TRUE) != FALSE;
    for (const auto& [key, count] : addresses) {
        const auto [phase, address] = key;
        alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> buffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        std::string name;
        if (symbols && SymFromAddr(process, address, &displacement, symbol)) { name = symbol->Name; }
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line); DWORD lineDisplacement = 0;
        const bool hasLine = symbols && SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != FALSE;
        MEMORY_BASIC_INFORMATION memory{};
        VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory));
        std::array<wchar_t, MAX_PATH> module{};
        GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), module.data(), static_cast<DWORD>(module.size()));
        Json row{{"phase", names[phase]}, {"address", address}, {"count", count}, {"symbol", name},
            {"symbol_displacement", displacement}, {"module", std::filesystem::path(module.data()).string()},
            {"rva", address - reinterpret_cast<uint64_t>(memory.AllocationBase)}};
        if (hasLine) { row.update({{"source", line.FileName}, {"line", line.LineNumber}}); }
        ips.push_back(std::move(row));
    }
    if (symbols) { SymCleanup(process); }
    return {{"resource_samples", samples}, {"instruction_samples", ips},
        {"sampler_cpu_ms", static_cast<double>(samplingCpu) / 10000.0}, {"suspend_failures", suspendFailures},
        {"thread_inventories", inventories}, {"inventory_wall_ms", inventoryMs}, {"context_wall_ms", contextMs}};
}
} // namespace obj_profile
