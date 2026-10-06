#include <prototype.hpp>
#include <nlohmann/json.hpp>
#include <iostream>

namespace {
struct Fingerprint { uint64_t value = 14695981039346656037ull; };
template <typename T> void hash(Fingerprint& result, const T& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) { result.value = (result.value ^ bytes[i]) * 1099511628211ull; }
}
template <typename T> void hashValues(Fingerprint& result, const T& values) {
    hash(result, values.size());
    for (const auto& value : values) { hash(result, value); }
}
int run(bool prototype, const std::filesystem::path& path, size_t chunkBytes, size_t workers) {
    try {
        rapidobj::PrototypeOptions options;
        if (chunkBytes) { options.chunk_bytes = chunkBytes; }
        options.workers = workers;
        parser_profile::worker_limit = static_cast<unsigned>(workers);
        std::vector<std::array<double, 3>> positions, normals;
        std::vector<std::array<double, 2>> texcoords;
        rapidobj::PrototypeResult result;
        const auto start = parser_profile::Clock::now();
        if (prototype) { result = rapidobj::ParseFilePrototype(path, {positions, texcoords, normals}, options); }
        else { result.polygons = rapidobj::ParseFile(path, rapidobj::MaterialLibrary::Default(rapidobj::Load::Optional)); }
        const auto total = parser_profile::elapsed(start);
        if (result.polygons.error) {
            throw std::runtime_error(result.polygons.error.code.message() + " at line " + std::to_string(result.polygons.error.line_num));
        }
        Fingerprint fingerprint;
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
            faces += shape.mesh.num_face_vertices.size(); lineIndices += shape.lines.indices.size(); pointIndices += shape.points.indices.size();
        }
        nlohmann::json output{{"total_ms", total}, {"scan_ms", parser_profile::scan_ms}, {"merge_ms", parser_profile::merge_ms},
            {"attribute_allocate_ms", parser_profile::allocate_ms}, {"merge_execute_ms", parser_profile::copy_ms},
            {"residual_ms", total-parser_profile::scan_ms-parser_profile::merge_ms},
            {"workers", parser_profile::workers}, {"chunks", parser_profile::chunks},
            {"worker_input_ms_sum", parser_profile::input_ns.load() / 1e6},
            {"worker_decode_ms_sum", parser_profile::decode_ns.load() / 1e6},
            {"positions", count}, {"faces", faces}, {"line_indices", lineIndices}, {"point_indices", pointIndices},
            {"fingerprint", fingerprint.value}};
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    const bool prototype = argc > 1 && std::wstring_view(argv[1]) == L"prototype";
    const bool legacy = argc > 1 && std::wstring_view(argv[1]) == L"legacy";
#else
int main(int argc, char** argv) {
    const bool prototype = argc > 1 && std::string_view(argv[1]) == "prototype";
    const bool legacy = argc > 1 && std::string_view(argv[1]) == "legacy";
#endif
    if (argc < 3 || argc > 5 || (!prototype && !legacy)) {
        std::cerr << "Usage: woby_parser_benchmark legacy|prototype path [chunk bytes] [workers]\n";
        return 2;
    }
    try {
        return run(prototype, std::filesystem::path(argv[2]), argc > 3 ? std::stoull(argv[3]) : 0, argc > 4 ? std::stoull(argv[4]) : 0);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
