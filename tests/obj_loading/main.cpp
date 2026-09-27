#include "probe.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include <iostream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

int main(int argc, char** argv)
{
    using namespace obj_probe;
    Json result;
    try {
        if (argc < 3 || argc > 4) { throw std::runtime_error("usage: benchmark BACKEND ABSOLUTE_OBJ_PATH [THREADS]"); }
        const std::string backend = argv[1];
        const auto path = woby::pathFromUtf8(argv[2]);
        if (!path.is_absolute()) { throw std::runtime_error("path must be absolute"); }
        const int threads = argc == 4 ? std::stoi(argv[3]) : -1;
        if (threads < -1) { throw std::runtime_error("invalid thread count"); }
        result = {{"backend", backend}, {"file", woby::pathToUtf8(path)}, {"bytes", std::filesystem::file_size(path)},
            {"rapid_io", OBJ_PROBE_IO}, {"hardware_threads", std::thread::hardware_concurrency()}, {"requested_threads", threads}};
        Json measured;
        if (backend == "rapid") { measured = rapid(path); }
        else if (backend == "fast") { measured = fast(argv[2]); }
        else if (backend == "tiny" || backend == "tiny-opt" || backend == "tiny-typed"
            || backend == "tiny-opt-cache" || backend == "tiny-typed-cache") { measured = tiny(argv[2], backend, threads); }
        else if (backend == "woby") {
            const auto begin = Clock::now();
            auto mesh = woby::loadObjMesh(path);
            measured = {{"load_ms", elapsed(begin)}, {"stages", stages}, {"vertices", mesh.vertices.size()},
                {"triangles", mesh.indices.size() / 3}, {"positions", mesh.sourceData->points.size()}, {"shapes", mesh.nodes.size()}};
        } else { throw std::runtime_error("unknown backend"); }
        result.update(measured);
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            result["peak_working_set_bytes"] = memory.PeakWorkingSetSize;
            result["peak_commit_bytes"] = memory.PeakPagefileUsage;
        }
#endif
        result["ok"] = true;
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        result["ok"] = false;
        result["error"] = error.what();
        std::cout << result.dump() << '\n';
        return 1;
    }
}
