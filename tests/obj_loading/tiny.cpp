#include "probe.h"
#define TINYOBJLOADER_IMPLEMENTATION
#define TINYOBJLOADER_USE_MULTITHREADING
#define TINYOBJLOADER_USE_SIMD
#define TINYOBJLOADER_USE_MMAP
#define TINYOBJLOADER_ENABLE_EXCEPTION
// The standard API otherwise rejects all supplied models at its 256 MiB cap.
// Keep a finite cap above the largest 7.1 GB fixture (64-bit benchmark only).
#define TINYOBJLOADER_STREAM_READER_MAX_BYTES (size_t(16) * 1024 * 1024 * 1024)
#include <tiny_obj_loader.h>

namespace obj_probe {
template <typename Attributes, typename Shapes>
Summary summarize(const Attributes& attrib, const Shapes& shapes)
{
    Summary s;
    s.normals = attrib.normals.size() / 3;
    s.texcoords = attrib.texcoords.size() / 2;
    for (size_t i = 0; i < attrib.vertices.size(); i += 3) { point(s, &attrib.vertices[i]); }
    for (const auto& shape : shapes) {
        for (const auto n : shape.mesh.num_face_vertices) { face(s, static_cast<size_t>(n)); }
        for (const auto& v : shape.mesh.indices) { index(s, v.vertex_index, v.normal_index, v.texcoord_index); }
    }
    return s;
}
Json tiny(const std::string& path, const std::string& mode, int threads)
{
    tinyobj::OptLoadConfig config;
    config.triangulate = false;
    config.num_threads = threads;
    config.float_cache = mode.find("-cache") != std::string::npos;
    std::string warn, error;
    Json result;
    const auto begin = Clock::now();
    if (mode == "tiny") {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        const auto base = std::filesystem::path(path).parent_path().string() + "/";
        const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &error, path.c_str(), base.c_str(), false, false);
        result["load_ms"] = elapsed(begin);
        if (!ok) { throw std::runtime_error(error); }
        result["geometry"] = summary(summarize(attrib, shapes));
        result["shapes"] = shapes.size();
        result["materials"] = materials.size();
    } else if (mode.find("tiny-typed") == 0) {
        auto mesh = tinyobj::LoadObjOptTyped(path.c_str(), &warn, &error, nullptr, config);
        result["load_ms"] = elapsed(begin);
        if (!mesh.valid) { throw std::runtime_error(error); }
        Summary s;
        s.normals = mesh.attrib.normals.size() / 3;
        s.texcoords = mesh.attrib.texcoords.size() / 2;
        for (size_t i = 0; i < mesh.attrib.vertices.size(); i += 3) { point(s, &mesh.attrib.vertices[i]); }
        for (const auto n : mesh.attrib.face_num_verts) { face(s, static_cast<size_t>(n)); }
        for (const auto& v : mesh.attrib.indices) { index(s, v.vertex_index, v.normal_index, v.texcoord_index); }
        result["geometry"] = summary(s);
        result["shapes"] = mesh.shapes.size();
        result["materials"] = mesh.materials.size();
    } else {
        tinyobj::basic_attrib_t<> attrib;
        std::vector<tinyobj::basic_shape_t<>> shapes;
        std::vector<tinyobj::material_t> materials;
        const bool ok = tinyobj::LoadObjOpt(&attrib, &shapes, &materials, &warn, &error, path.c_str(), nullptr, config);
        result["load_ms"] = elapsed(begin);
        if (!ok) { throw std::runtime_error(error); }
        result["geometry"] = summary(summarize(attrib, shapes));
        result["shapes"] = shapes.size();
        result["materials"] = materials.size();
    }
    result["warning"] = warn;
    result["float_cache"] = config.float_cache;
#ifdef TINYOBJLOADER_SIMD_AVX2
    result["simd"] = "AVX2";
#elif defined(TINYOBJLOADER_SIMD_SSE2)
    result["simd"] = "SSE2";
#else
    result["simd"] = "other";
#endif
    return result;
}
} // namespace obj_probe
