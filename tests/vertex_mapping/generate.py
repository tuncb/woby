"""Generate instrumented production copies, failing on stale anchors."""
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)


def replace(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


for mode in ("baseline", "hybrid", "adaptive"):
    text = (root / "src/obj_mesh.cpp").read_text()
    text = '#include "mapping_probe.h"\n#include <bit>\n' + text
    if mode == "baseline":
        start = text.index("struct VertexIndexTable {")
        end = text.index("rapidobj::Result parseObj", start)
        text = text[:start] + (root / "tests/vertex_mapping/legacy.h").read_text() + "\n" + text[end:]
        text = replace(text, "reserveIndexTable(vertexMap, source->points.size(), indexCount,\n        attrib.normals.empty() && attrib.texcoords.empty());",
                       "reserveIndexTable(vertexMap, vertexCapacity);")
    elif mode == "hybrid":
        text = replace(text, "attrib.normals.empty() && attrib.texcoords.empty()", "false")
    text = replace(text, "auto result = parseObj(path);", 'mapping_probe::start();\n    auto result = parseObj(path);\n    mapping_probe::mark("parse_ms");')
    text = replace(text, "const auto& attrib = result.attributes;", 'mapping_probe::mark("triangulate_ms");\n    const auto& attrib = result.attributes;')
    text = replace(text, "VertexIndexTable vertexMap;", 'mapping_probe::mark("source_copy_ms");\n    VertexIndexTable vertexMap;')
    text = replace(text, "    for (size_t shapeIndex", '    mapping_probe::mark("map_allocate_ms");\n    for (size_t shapeIndex')
    metadata = '''
    mapping_probe::mark("map_loop_ms");
    mapping_probe::stats = {{"position_count", attrib.positions.size()/3},
        {"normal_count", attrib.normals.size()/3}, {"uv_count", attrib.texcoords.size()/2},
        {"key_capacity_bytes", vertexMap.keys.capacity()*sizeof(IndexKey)},
        {"bucket_bytes", vertexMap.buckets.size()*sizeof(uint32_t)}};
'''
    if mode != "baseline":
        metadata += '''
    mapping_probe::stats["primary_bytes"] = vertexMap.primary.size()*sizeof(uint32_t);
    mapping_probe::stats["secondary_entries"] = vertexMap.secondaryCount;
    mapping_probe::stats["direct"] = vertexMap.direct;
'''
    text = replace(text, "    mesh.sourceData = std::move(source);", metadata + "    mesh.sourceData = std::move(source);")
    text = replace(text, "    return mesh;", '''
    mapping_probe::mark("metadata_and_finalize_ms");
    vertexMap = {};
    result = {};
    mapping_probe::mark("temporary_release_ms");
    return mesh;''')
    (output / f"obj_{mode}.cpp").write_text(text)

text = '#include "mapping_probe.h"\n' + (root / "src/model_mesh.cpp").read_text()
text = replace(text, "    if (generateMissingSmoothNormals && !hasCompleteNormals(mesh.vertices)) {", '''
    mapping_probe::previous = mapping_probe::Clock::now();
    const bool missing = generateMissingSmoothNormals && !hasCompleteNormals(mesh.vertices);
    mapping_probe::mark("normal_check_ms");
    if (missing) {''')
text = replace(text, "    mesh.bounds = calculateBounds(mesh.vertices);", '''
    mapping_probe::mark("normal_generate_ms");
    mesh.bounds = calculateBounds(mesh.vertices);
    mapping_probe::mark("bounds_ms");''')
(output / "model_mesh.cpp").write_text(text)

text = (root / "src/surface_annotation.cpp").read_text()
start = text.index("void prepareAnnotationMeshCache(Mesh& mesh)")
end = text.index("std::string gestureFingerprint", start)
(output / "annotation_cache.cpp").write_text('''#include "model_mesh.h"
#include "hash_utils.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace woby {
''' + text[start:end] + '\n}\n')

text = (root / "src/ui_state.cpp").read_text()
start = text.index("bool boundsContainFinitePoints(")
end = text.index("void expandTransformedBounds(", start)
helpers = text[start:end]
start = text.index("std::array<float, 4> defaultGroupColor(")
end = text.index("UiFileState createUiFileState(", start)
(output / "group_state.cpp").write_text('''#include "ui_state.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace woby {
''' + helpers + text[start:end] + '\n}\n')

# Exact production CPU range construction, excluding graphics API calls.
text = (root / "src/scene_renderer.cpp").read_text()
start = text.index("uint32_t appendPointIndicesForRange(")
end = text.index("void buildPointSprites(", start)
helper = text[start:end]
start = text.index("    GpuMesh gpuMesh;", text.index("GpuMesh createGpuMesh("))
end = text.index("    try {", start)
body = text[start:end]
body = body.replace("    const auto vertexBytes = sceneBufferBytes(mesh.vertices.size(), sizeof(Vertex));", "")
body = body.replace("    const auto indexBytes = sceneBufferBytes(mesh.indices.size(), sizeof(uint32_t));", "")
body = replace(body, "    gpuMesh.nodeRanges.reserve", '    mapping_probe::mark("gpu_index_validate_ms");\n    gpuMesh.nodeRanges.reserve')
body = replace(body, "    for (size_t nodeIndex", '    mapping_probe::mark("point_allocate_ms");\n    for (size_t nodeIndex')
(output / "point_ranges.cpp").write_text('''#include "mapping_probe.h"
#include "scene_renderer.h"
#include <limits>
namespace woby {
uint32_t sceneBufferBytes(size_t count, size_t bytes) {
    if (count > std::numeric_limits<uint32_t>::max()/bytes) { throw std::runtime_error("buffer too large"); }
    return static_cast<uint32_t>(count*bytes);
}
''' + helper + '\nGpuMesh mappingPointRanges(const Mesh& mesh) {\n' + body + '''
    mapping_probe::mark("point_ranges_ms");
    return gpuMesh;
}
}
''')
