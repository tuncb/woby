#include <prototype.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <psapi.h>
#endif

namespace {
struct Resources { double cpuMs = 0; size_t workingSet = 0, privateBytes = 0, peakWorkingSet = 0; uint64_t readBytes = 0; };
Resources resources() {
    Resources result;
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        result.cpuMs = static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        result.workingSet = memory.WorkingSetSize; result.privateBytes = memory.PrivateUsage; result.peakWorkingSet = memory.PeakWorkingSetSize;
    }
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) { result.readBytes = io.ReadTransferCount; }
#endif
    return result;
}
struct Fingerprint { uint64_t value = 14695981039346656037ull; };
template <typename T> void hash(Fingerprint& result, const T& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) { result.value = (result.value ^ bytes[i]) * 1099511628211ull; }
}
template <typename T> void hashValues(Fingerprint& result, const T& values) {
    hash(result, values.size());
    for (const auto& value : values) { hash(result, value); }
}
#ifdef _WIN32
double cpuMilliseconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("Cannot read process CPU time.");
    }
    const auto ticks = [](FILETIME time) { return (uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
}
#endif
int run(bool prototype, bool readColors, bool memoryInput, const std::filesystem::path& path, size_t chunkBytes, size_t workers) {
    try {
        rapidobj::PrototypeOptions options;
        if (chunkBytes) { options.chunk_bytes = chunkBytes; }
        options.workers = workers;
        parser_profile::worker_limit = static_cast<unsigned>(workers);
        std::vector<std::array<double, 3>> positions, normals;
        std::vector<std::array<double, 2>> texcoords;
        std::vector<uint32_t> colors;
        rapidobj::PrototypeResult result;
        std::string text;
        const auto inputStart = parser_profile::Clock::now();
        if (memoryInput) {
            const auto size = std::filesystem::file_size(path);
            if (size > text.max_size()) { throw std::length_error("Input exceeds string capacity"); }
            text.resize(static_cast<size_t>(size));
            std::ifstream input(path, std::ios::binary);
            for (size_t offset = 0; offset < text.size();) {
                const auto bytes = std::min(size_t{64 * 1024 * 1024}, text.size() - offset);
                if (!input.read(text.data() + offset, static_cast<std::streamsize>(bytes))) {
                    throw std::runtime_error("Cannot read memory benchmark input");
                }
                offset += bytes;
            }
        }
        const auto inputPrepare = parser_profile::elapsed(inputStart);
        const auto before = resources();
#ifdef _WIN32
        const auto cpuStart = cpuMilliseconds();
#endif
        const auto start = parser_profile::Clock::now();
        if (memoryInput) {
            result = rapidobj::ParseMemoryPrototype(text, {positions, texcoords, normals, readColors ? &colors : nullptr},
                rapidobj::MaterialLibrary::SearchPath(std::filesystem::absolute(path).parent_path(), rapidobj::Load::Optional), options);
        }
        else if (prototype) { result = rapidobj::ParseFilePrototype(path, {positions, texcoords, normals, readColors ? &colors : nullptr}, options); }
        else { result.polygons = rapidobj::ParseFile(path, rapidobj::MaterialLibrary::Default(rapidobj::Load::Optional)); }
        const auto total = parser_profile::elapsed(start);
#ifdef _WIN32
        const auto cpuMs = cpuMilliseconds() - cpuStart;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            throw std::runtime_error("Cannot read process memory use.");
        }
#endif
        const auto after = resources();
        if (result.polygons.error) {
            throw std::runtime_error(result.polygons.error.code.message() + " at line " + std::to_string(result.polygons.error.line_num));
        }
        Fingerprint fingerprint;
        Fingerprint precise;
        if (prototype) {
            hashValues(precise, positions); hashValues(precise, texcoords); hashValues(precise, normals);
        }
        const auto& attributes = result.polygons.attributes;
        const auto count = prototype ? positions.size() : attributes.positions.size() / 3;
        hash(fingerprint, count);
        if (prototype) { for (const auto& tuple : positions) { for (double value : tuple) { hash(fingerprint, value); } } }
        else { for (double value : attributes.positions) { hash(fingerprint, value); } }
        hash(fingerprint, prototype ? texcoords.size() * 2 : attributes.texcoords.size());
        if (prototype) { for (const auto& tuple : texcoords) { for (double value : tuple) { hash(fingerprint, static_cast<float>(value)); } } }
        else { for (float value : attributes.texcoords) { hash(fingerprint, value); } }
        hash(fingerprint, prototype ? normals.size() * 3 : attributes.normals.size());
        if (prototype) { for (const auto& tuple : normals) { for (double value : tuple) { hash(fingerprint, static_cast<float>(value)); } } }
        else { for (float value : attributes.normals) { hash(fingerprint, value); } }
        size_t faces = 0, lineIndices = 0, pointIndices = 0;
        for (const auto& shape : result.polygons.shapes) {
            hashValues(fingerprint, shape.name);
            hashValues(fingerprint, shape.mesh.indices); hashValues(fingerprint, shape.mesh.num_face_vertices);
            hashValues(fingerprint, shape.mesh.material_ids); hashValues(fingerprint, shape.mesh.smoothing_group_ids);
            hashValues(fingerprint, shape.lines.indices); hashValues(fingerprint, shape.lines.num_line_vertices);
            hashValues(fingerprint, shape.points.indices);
            hashValues(precise, shape.name);
            hashValues(precise, shape.mesh.indices); hashValues(precise, shape.mesh.num_face_vertices);
            hashValues(precise, shape.mesh.material_ids); hashValues(precise, shape.mesh.smoothing_group_ids);
            hashValues(precise, shape.lines.indices); hashValues(precise, shape.lines.num_line_vertices);
            hashValues(precise, shape.points.indices);
            faces += shape.mesh.num_face_vertices.size(); lineIndices += shape.lines.indices.size(); pointIndices += shape.points.indices.size();
        }
        nlohmann::json output{{"total_ms", total}, {"scan_ms", parser_profile::scan_ms}, {"merge_ms", parser_profile::merge_ms},
            {"attribute_allocate_ms", parser_profile::allocate_ms}, {"merge_execute_ms", parser_profile::copy_ms},
            {"residual_ms", total-parser_profile::scan_ms-parser_profile::merge_ms},
            {"workers", parser_profile::workers}, {"chunks", parser_profile::chunks},
            {"worker_input_ms_sum", parser_profile::input_ns.load() / 1e6},
            {"worker_decode_ms_sum", parser_profile::decode_ns.load() / 1e6},
            {"positions", count}, {"faces", faces}, {"line_indices", lineIndices}, {"point_indices", pointIndices},
            {"input_kind", memoryInput ? "preloaded-memory" : "file"}, {"input_prepare_ms", inputPrepare},
            {"retained_input_bytes", text.size()},
            {"parse_cpu_ms", after.cpuMs - before.cpuMs}, {"average_logical_cpus", (after.cpuMs - before.cpuMs) / total},
            {"working_set_after_parse", after.workingSet}, {"private_bytes_after_parse", after.privateBytes},
            {"peak_working_set_after_parse", after.peakWorkingSet}, {"parse_read_bytes", after.readBytes - before.readBytes},
            {"fingerprint", fingerprint.value}};
        output["precise_fingerprint"] = precise.value;
        Fingerprint colorFingerprint; hashValues(colorFingerprint, colors);
        output["vertex_color_count"] = colors.size();
        output["vertex_color_bytes"] = colors.size() * sizeof(uint32_t);
        output["color_fingerprint"] = colorFingerprint.value;
#ifdef _WIN32
        output["cpu_ms"] = cpuMs;
        output["peak_working_set_bytes"] = memory.PeakWorkingSetSize;
        output["peak_commit_bytes"] = memory.PeakPagefileUsage;
#endif
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    const bool readColors = argc > 1 && std::wstring_view(argv[1]) == L"prototype-colors";
    const bool memoryInput = argc > 1 && std::wstring_view(argv[1]) == L"prototype-memory";
    const bool prototype = readColors || memoryInput || (argc > 1 && std::wstring_view(argv[1]) == L"prototype");
    const bool legacy = argc > 1 && std::wstring_view(argv[1]) == L"legacy";
#else
int main(int argc, char** argv) {
    const bool readColors = argc > 1 && std::string_view(argv[1]) == "prototype-colors";
    const bool memoryInput = argc > 1 && std::string_view(argv[1]) == "prototype-memory";
    const bool prototype = readColors || memoryInput || (argc > 1 && std::string_view(argv[1]) == "prototype");
    const bool legacy = argc > 1 && std::string_view(argv[1]) == "legacy";
#endif
    if (argc < 3 || argc > 5 || (!prototype && !legacy)) {
        std::cerr << "Usage: woby_parser_benchmark legacy|prototype|prototype-colors|prototype-memory path [chunk bytes] [workers]\n";
        return 2;
    }
    try {
        return run(prototype, readColors, memoryInput, std::filesystem::path(argv[2]), argc > 3 ? std::stoull(argv[3]) : 0, argc > 4 ? std::stoull(argv[4]) : 0);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
