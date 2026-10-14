"""Generate controlled corner mapping alternatives from the current loader."""
import importlib.util
import os
from pathlib import Path
import sys


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Corner mapping anchor drift ({text.count(old)} matches): {old[:100]!r}")
    return text.replace(old, new, 1)


def write_changed(path, text):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def generate(root, output):
    spec = importlib.util.spec_from_file_location("obj_profile_generator", root / "experiments/obj-profile/generate.py")
    profile = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(profile)
    previous = {path: (path.read_bytes(), path.stat().st_atime_ns, path.stat().st_mtime_ns)
                for path in (output / "base").rglob("*") if path.is_file()}
    profile.generate(root, output / "base", reference=True)
    for path, (content, atime, mtime) in previous.items():
        if path.read_bytes() == content:
            os.utime(path, ns=(atime, mtime))
    baseline = (output / "base/obj_mesh.cpp").read_text(encoding="utf-8")
    # Keep the research reference at its original fused mapper after the
    # staged design becomes the production default.
    baseline = replace(baseline, "const bool staged = stagedCornerMapping", "const bool staged = false && stagedCornerMapping")
    baseline = replace(baseline, '    obj_profile::next(measuredMesh, "corner_mapping");', '''    size_t parserTriangleBytes = 0, parserFaceMetadataBytes = 0;
    for (const auto& shape : shapes) {
        parserTriangleBytes += shape.mesh.indices.size() * sizeof(rapidobj::Index);
        parserFaceMetadataBytes += shape.mesh.num_face_vertices.size() * sizeof(uint8_t)
            + shape.mesh.material_ids.size() * sizeof(int32_t)
            + shape.mesh.smoothing_group_ids.size() * sizeof(uint32_t);
    }
    obj_profile::next(measuredMesh, "corner_mapping", {
        {"parser_triangle_bytes", parserTriangleBytes}, {"parser_face_metadata_bytes", parserFaceMetadataBytes},
        {"input_uv_capacity_bytes", Prototype ? buffers->texcoords.capacity() * sizeof(std::array<double, 2>) : attrib.texcoords.size() * sizeof(float)},
        {"input_normal_capacity_bytes", Prototype ? buffers->normals.capacity() * sizeof(Coordinate) : attrib.normals.size() * sizeof(float)},
        {"source_position_capacity_bytes", sourcePositions(*source).capacity() * sizeof(Coordinate)},
        {"initial_vertex_capacity_bytes", meshData.vertices.capacity() * sizeof(Vertex)},
        {"render_index_capacity_bytes", meshData.indices.capacity() * sizeof(uint32_t)},
        {"source_index_capacity_bytes", source->indices.capacity() * sizeof(uint32_t)}});''')
    write_changed(output / "obj_baseline.cpp", baseline)
    write_changed(output / "obj_pointer.cpp", baseline)
    text = '#include "mapping.h"\n#include <atomic>\n#include <exception>\n#include <thread>\n' + baseline
    start = text.index("// First-use IDs are global across shapes.")
    end = text.index("rapidobj::Result parseObj", start)
    table = (root / "experiments/corner-mapping/index_table.inc").read_text(encoding="utf-8")
    helpers = (root / "experiments/corner-mapping/split_mapping.inc").read_text(encoding="utf-8")
    release = (root / "experiments/corner-mapping/release_mapping.inc").read_text(encoding="utf-8")
    text = text[:start] + table + "\n" + helpers + "\n" + release + "\n" + text[end:]
    text = replace(text, "    meshData.vertices.reserve(vertexCapacity);", "    if (indexCount == 0 || (Prototype ? buffers->normals.empty() && buffers->texcoords.empty() : attrib.normals.empty() && attrib.texcoords.empty())) {\n        meshData.vertices.reserve(vertexCapacity);\n    }")
    text = replace(text, "    reserveIndexTable(vertexMap, sourcePositions(*source).size(), vertexCapacity,", "    vertexMap.preciseUvs = Prototype ? buffers->texcoords.data() : nullptr;\n    vertexMap.floatUvs = attrib.texcoords.data();\n    reserveIndexTable(vertexMap, sourcePositions(*source).size(), vertexCapacity,")
    text = replace(text,
        "        Prototype ? buffers->normals.empty() && buffers->texcoords.empty() : attrib.normals.empty() && attrib.texcoords.empty());",
        "        indexCount == 0 || (Prototype ? buffers->normals.empty() && buffers->texcoords.empty() : attrib.normals.empty() && attrib.texcoords.empty()));")
    start = text.index("    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {", text.index("Mesh buildObjMesh("))
    end = text.index("    cornerMetrics.primaryCapacityBytes", start)
    original_loop = text[start:end]
    original_loop = replace(original_loop, "const auto mappedIndex = vertexIndex(vertexMap, key);", "const auto mappedIndex = vertexIndex(vertexMap, key) & renderIdMask;")
    replacement = """    if (!vertexMap.direct) {
        resolveSeparateFaces<Prototype>(shapes, vertexMap, meshData, *source, buffers, attrib, progress, totalIndices, deferPreparation);
#ifdef CORNER_MAPPING_RELEASE
        releaseResolvedTriangles(result, vertexMap, indexCount, progress, totalIndices);
#endif
        gatherResolvedVertices<Prototype>(meshData, vertexMap.keys, sourcePositions(*source), buffers, attrib, progress, totalIndices);
    } else {
""" + original_loop + "    }\n\n"
    text = text[:start] + replacement + text[end:]
    text = replace(text, "const auto mapped = vertexIndex(vertexMap, {positionIndex, -1, -1});", "const auto mapped = vertexIndex(vertexMap, {positionIndex, -1, -1}) & renderIdMask;")
    text = replace(text, '{"primary_bytes", vertexMap.primary.size()*sizeof(uint32_t)}', '{"primary_bytes", vertexMap.primary.size()*sizeof(uint32_t) + vertexMap.entries.size()*sizeof(PrimaryEntry)}')
    text = replace(text, '{"direct", vertexMap.direct}};', '{"direct", vertexMap.direct},\n        {"dense_primary_capacity_bytes", vertexMap.entries.capacity()*sizeof(PrimaryEntry)},\n        {"extra_primary_capacity_bytes", vertexMap.entries.capacity()*(sizeof(PrimaryEntry)-sizeof(uint32_t))},\n        {"extra_corner_buffers_bytes", 0}, {"extra_vertex_keys_bytes", 0},\n        {"uv_flags_bytes", 0}, {"gather_workers_requested", corner_mapping::gatherWorkers},\n        {"unique_uv_validation", corner_mapping::validateUniqueUvs}};')
    write_changed(output / "obj_split.cpp", text)
    write_changed(output / "obj_dense.cpp", text)
    write_changed(output / "obj_release.cpp", text)
    compact = text
    start = compact.index("// This file is inserted into")
    end = compact.index("rapidobj::Result parseObj", start)
    resolver = helpers[helpers.index("template <bool Prototype>\n__declspec(noinline) void resolveSeparateFaces"):]
    resolver = resolver.replace('{"vertices", table.keys.size()}', '{"vertices", table.next}')
    compact_table = (root / "experiments/corner-mapping/compact_table.inc").read_text(encoding="utf-8")
    compact_gather = (root / "experiments/corner-mapping/compact_mapping.inc").read_text(encoding="utf-8")
    compact = compact[:start] + compact_table + "\n" + compact_gather + "\n" + resolver + "\n" + release + "\n" + compact[end:]
    compact = replace(compact, "gatherResolvedVertices<Prototype>(meshData, vertexMap.keys,", "gatherResolvedVertices<Prototype>(meshData, vertexMap,")
    compact = replace(compact, "vertexMap.keys.capacity()*sizeof(IndexKey)", "vertexMap.keys.capacity()*sizeof(SecondaryEntry)")
    write_changed(output / "obj_compact.cpp", compact)
    write_changed(output / "obj_compact_release.cpp", compact)
    fast_resolver = (root / "experiments/corner-mapping/fast_resolve.inc").read_text(encoding="utf-8")
    fast = replace(compact, resolver, fast_resolver)
    write_changed(output / "obj_fast.cpp", fast)
    write_changed(output / "obj_fast_release.cpp", fast)


if __name__ == "__main__":
    generate(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve())
