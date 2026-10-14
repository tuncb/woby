"""Add coarse measurements to generated copies. Fail if a source anchor changes."""
import importlib.util
import sys
from pathlib import Path


def replace(text, old, new):
    count = text.count(old)
    if count != 1:
        raise ValueError(f"Profile anchor drift ({count} matches): {old[:100]!r}")
    return text.replace(old, new, 1)


def section(text, first, last, changes):
    start = text.index(first)
    end = text.index(last, start)
    part = text[start:end]
    for old, new in changes:
        part = replace(part, old, new)
    return text[:start] + part + text[end:]


def generate(root, output, reference=False):
    if reference:
        spec = importlib.util.spec_from_file_location("reference_profile_generator", root / "experiments/obj-mapping-reference/profile_generate.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.generate(root, output)
        return
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("parser_generator", root / "experiments/parser-throughput/generate.py")
    parser = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(parser)
    headers = output / "rapidobj"
    parser.generate(root, headers)
    text = (headers / "rapidobj.hpp").read_text(encoding="utf-8")
    text = '#include "profile.h"\n' + text
    text = replace(text, '    // compute overall sizes of lists',
                   '    auto measuredMerge = obj_profile::begin("parser_merge_shape_plan");\n    // compute overall sizes of lists')
    text = replace(text, '    const auto allocation_started = parser_profile::Clock::now();',
                   '    obj_profile::next(measuredMerge, "parser_attribute_allocation");\n    const auto allocation_started = parser_profile::Clock::now();')
    text = replace(text, '    // compute tasks to construct attribute arrays',
                   '    obj_profile::next(measuredMerge, "parser_merge_attribute_plan");\n    // compute tasks to construct attribute arrays')
    text = replace(text, '    const auto copy_started = parser_profile::Clock::now();',
                   '    obj_profile::next(measuredMerge, "parser_merge_execute");\n    const auto copy_started = parser_profile::Clock::now();')
    text = replace(text, '    parser_profile::copy_ms = parser_profile::elapsed(copy_started);',
                   '    parser_profile::copy_ms = parser_profile::elapsed(copy_started);\n    obj_profile::end(measuredMerge);')
    (headers / "rapidobj.hpp").write_text(text, encoding="utf-8")
    text = (headers / "prototype.hpp").read_text(encoding="utf-8")
    text = replace(text, '    const auto range_started = parser_profile::Clock::now();',
                   '    const auto range_started = parser_profile::Clock::now();\n    const auto workerTrace = obj_profile::traceBegin("parser_worker_read_decode");\n    const auto workerCpuStart = obj_profile::threadCpu();')
    text = replace(text, '    parser_profile::decode_ns.fetch_add(parser_profile::nanoseconds(range_started)-range_io.count(), std::memory_order_relaxed);',
                   '    parser_profile::decode_ns.fetch_add(parser_profile::nanoseconds(range_started)-range_io.count(), std::memory_order_relaxed);\n    obj_profile::traceEnd(workerTrace);\n    obj_profile::worker(parser_profile::elapsed(range_started), std::chrono::duration<double, std::milli>(range_io).count(), workerCpuStart);')
    text = replace(text, '    const auto scan_started = parser_profile::Clock::now();',
                   '    auto measuredParser = obj_profile::begin("parser_scan_collect");\n    const auto scan_started = parser_profile::Clock::now();')
    text = replace(text, '        const auto merge_started = parser_profile::Clock::now();',
                   '        obj_profile::next(measuredParser, "parser_merge");\n        const auto merge_started = parser_profile::Clock::now();')
    text = replace(text, '        parser_profile::merge_ms = parser_profile::elapsed(merge_started);',
                   '        parser_profile::merge_ms = parser_profile::elapsed(merge_started);\n        obj_profile::end(measuredParser);')
    (headers / "prototype.hpp").write_text(text, encoding="utf-8")

    text = (root / "src/obj_mesh.cpp").read_text(encoding="utf-8")
    text = section(text, "Mesh buildObjMesh(", "} // namespace", [
        ('    const auto throwLoadError', '    auto measuredMesh = obj_profile::begin("primitive_validation");\n    const auto throwLoadError'),
        ('    std::vector<Coordinate> sourcePoints;', '    obj_profile::next(measuredMesh, "source_position_adoption");\n    std::vector<Coordinate> sourcePoints;'),
        ('    const auto origin = coordinateOrigin(sourcePoints);', '    obj_profile::next(measuredMesh, "coordinate_origin");\n    const auto origin = coordinateOrigin(sourcePoints);\n    obj_profile::next(measuredMesh, "coordinate_rebase");'),
        ('    reportModelLoadProgress(progress, ModelLoadStage::triangulating);', '    obj_profile::next(measuredMesh, "triangulation");\n    reportModelLoadProgress(progress, ModelLoadStage::triangulating);'),
        ('    const auto& shapes = result.shapes;', '    obj_profile::next(measuredMesh, "mesh_allocation");\n    const auto& shapes = result.shapes;'),
        ('    ObjCornerMappingMetrics cornerMetrics;', '    obj_profile::next(measuredMesh, "corner_mapping");\n    ObjCornerMappingMetrics cornerMetrics;'),
        ('    mesh.sourceData = std::move(source);', '''    obj_profile::end(measuredMesh, {{"primary_bytes", cornerMetrics.primaryCapacityBytes},
        {"key_capacity_bytes", cornerMetrics.keyCapacityBytes}, {"bucket_bytes", cornerMetrics.bucketCapacityBytes},
        {"secondary_vertices", cornerMetrics.seamVertices}, {"vertex_capacity", meshData.vertices.capacity()},
        {"position_map_capacity", meshData.positionIndices.capacity()}, {"production_staged", cornerMetrics.staged},
        {"production_primary_capacity_bytes", cornerMetrics.primaryCapacityBytes},
        {"production_key_capacity_bytes", cornerMetrics.keyCapacityBytes},
        {"production_released_triangle_bytes", cornerMetrics.releasedTriangleBytes}, {"direct", true}});
    mesh.sourceData = std::move(source);'''),
        ('    publishMeshGeometry(mesh);', '    measuredMesh = obj_profile::begin("mesh_publish");\n    publishMeshGeometry(mesh);\n    obj_profile::next(measuredMesh, "parser_lookup_release");\n    result = {};\n    obj_profile::end(measuredMesh);'),
    ])
    text = section(text, 'Mesh buildObjPrototype(', '} // namespace', [
        ('    const auto start = Clock::now();', '    auto measuredLoad = obj_profile::begin("parser_total");\n    const auto start = Clock::now();'),
        ('    const auto parsedAt = Clock::now();', '    const auto parsedAt = Clock::now();\n    obj_profile::next(measuredLoad, "freeform_resolve");'),
        ('    const auto resolvedAt = Clock::now();', '    const auto resolvedAt = Clock::now();\n    obj_profile::next(measuredLoad, "statement_release");'),
        ('    auto mesh = buildObjMesh<true>', '    obj_profile::end(measuredLoad);\n    auto mesh = buildObjMesh<true>'),
    ])
    text = section(text, 'Mesh loadObjMeshPrototype(', 'Mesh loadObjMeshTextPrototype(', [
        ('    preflightObjCapacity(path, progress);', '    const auto measuredPreflight = obj_profile::begin("capacity_preflight");\n    preflightObjCapacity(path, progress);\n    obj_profile::end(measuredPreflight);'),
    ])
    (output / "obj_mesh.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    # Diagnostics count the common resolver, including explicit primitives.
    text = (root / "src/obj_corner_mapping.h").read_text(encoding="utf-8")
    text = replace(text, '    uint32_t next = 0;', '''    uint32_t next = 0;
#ifdef OBJ_PROFILE_DIAGNOSTIC
    uint64_t firstPositions = 0, primaryHits = 0, secondaryLookups = 0, secondaryHits = 0, probes = 0, growths = 0;
#endif''')
    text = replace(text, 'inline void growBuckets(Table& table) {', '''inline void growBuckets(Table& table) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
    ++table.growths;
#endif''')
    text = replace(text, '    while (table.buckets[bucket] != empty) {\n        const auto& seam', '''    while (table.buckets[bucket] != empty) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
        ++table.probes;
#endif
        const auto& seam''')
    text = replace(text, '        if (seam.key == key) { return seam.id; }', '''        if (seam.key == key) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
            ++table.secondaryHits;
#endif
            return seam.id;
        }''')
    text = replace(text, '\n    if (entry.id == empty) {', '''
    if (entry.id == empty) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
        ++table.firstPositions;
#endif''')
    text = replace(text, '    if (entry.normal == key.normal && entry.uv == key.uv) { return entry.id; }', '''    if (entry.normal == key.normal && entry.uv == key.uv) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
        ++table.primaryHits;
#endif
        return entry.id;
    }
#ifdef OBJ_PROFILE_DIAGNOSTIC
    ++table.secondaryLookups;
#endif''')
    text = replace(text, '            if (entry.id == empty) {', '''            if (entry.id == empty) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
                ++table.firstPositions;
#endif''')
    text = replace(text, '            } else if (entry.normal == key.normal && entry.uv == key.uv) { id = entry.id; }\n            else { id = seamId<Precise>(table,key,preciseUvs,floatUvs); }', '''            } else if (entry.normal == key.normal && entry.uv == key.uv) {
#ifdef OBJ_PROFILE_DIAGNOSTIC
                ++table.primaryHits;
#endif
                id = entry.id;
            } else {
#ifdef OBJ_PROFILE_DIAGNOSTIC
                ++table.secondaryLookups;
#endif
                id = seamId<Precise>(table,key,preciseUvs,floatUvs);
            }''')
    text = replace(text, '    table.seams.reserve(std::min({sourcePositions(source).size(),corners,size_t{1024}}));',
                   '    table.seams.reserve(obj_profile::reserveEstimate(std::min({sourcePositions(source).size(),corners,size_t{1024}}),corners));')
    text = replace(text, '    const bool allFiniteUvs = allUvsFinite<Precise>(preciseUvs,floatUvs,uvCount,progress,corners,totalIndices);', '''#ifdef OBJ_PROFILE_UV_CHECK
    const auto measuredUvCache = obj_profile::begin("uv_validation_cache");
#endif
    const bool allFiniteUvs = allUvsFinite<Precise>(preciseUvs,floatUvs,uvCount,progress,corners,totalIndices);
#ifdef OBJ_PROFILE_UV_CHECK
    obj_profile::end(measuredUvCache, {{"uvs", uvCount}, {"all_finite", allFiniteUvs}});
#endif''')
    text = replace(text, '    metrics.gatherWorkers = std::max(metrics.gatherWorkers,seamWorkers); metrics.seamGatherMs = elapsed(start);', '''    metrics.gatherWorkers = std::max(metrics.gatherWorkers,seamWorkers); metrics.seamGatherMs = elapsed(start);
#ifdef OBJ_PROFILE_DIAGNOSTIC
    const auto diagnostic = obj_profile::begin("mapping_staged_diagnostic");
    obj_profile::end(diagnostic, {{"first_positions", table.firstPositions}, {"primary_hits", table.primaryHits},
        {"secondary_lookups", table.secondaryLookups}, {"secondary_hits", table.secondaryHits},
        {"occupied_bucket_probes", table.probes}, {"hash_table_growths", table.growths}});
#endif''')
    (output / "obj_corner_mapping.h").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    text = (root / "src/model_mesh.cpp").read_text(encoding="utf-8")
    text = section(text, 'void generateSmoothNormals(', 'Bounds calculateBounds(', [
        ('    const auto total', '    auto measuredNormals = obj_profile::begin("normal_clear");\n    const auto total'),
        ('    for (size_t index = 0;', '    obj_profile::next(measuredNormals, "normal_face_accumulation");\n    for (size_t index = 0;'),
        ('    completed += indices.size();', '    obj_profile::next(measuredNormals, "normal_vertex_normalize");\n    completed += indices.size();'),
        ('    reportModelLoadProgress(progress, ModelLoadStage::normals, total, total);', '    reportModelLoadProgress(progress, ModelLoadStage::normals, total, total);\n    obj_profile::end(measuredNormals);'),
    ])
    text = section(text, 'void finalizeMesh(', '} // namespace woby', [
        ('    reportModelLoadProgress(progress, ModelLoadStage::normals);', '    auto measuredFinalize = obj_profile::begin("normal_check");\n    reportModelLoadProgress(progress, ModelLoadStage::normals);'),
        ('    if (generateMissingSmoothNormals && !hasCompleteNormals(meshGeometry(mesh).vertices)) {', '    const bool missing = generateMissingSmoothNormals && !hasCompleteNormals(meshGeometry(mesh).vertices);\n    obj_profile::next(measuredFinalize, "normal_generation");\n    if (missing) {'),
        ('    reportModelLoadProgress(progress, ModelLoadStage::bounds);', '    obj_profile::next(measuredFinalize, "render_bounds");\n    reportModelLoadProgress(progress, ModelLoadStage::bounds);'),
        ('    mesh.originalBounds.reset();', '    obj_profile::next(measuredFinalize, "original_bounds");\n    mesh.originalBounds.reset();'),
        ('    if (!mesh.preparationDeferred) { mesh.originalBounds = originalMeshBounds(mesh); }',
         '    if (!mesh.preparationDeferred) { mesh.originalBounds = originalMeshBounds(mesh); }\n    obj_profile::end(measuredFinalize);'),
    ])
    (output / "model_mesh.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")
    text = (root / "src/scene_mesh_preparation.cpp").read_text(encoding="utf-8")
    text = section(text, 'std::optional<SceneMeshPreparation> prepareSceneMesh(', '} // namespace woby', [
        ('    const auto canceled', '    auto measuredPrepare = obj_profile::begin("gpu_index_validation");\n    const auto canceled'),
        ('    SceneMeshPreparation result;', '    obj_profile::next(measuredPrepare, "point_membership_allocate");\n    SceneMeshPreparation result;'),
        ('    for (size_t nodeIndex = 0;', '    obj_profile::next(measuredPrepare, "point_membership_build");\n    for (size_t nodeIndex = 0;'),
        ('    if (buildPointHierarchy &&', '    obj_profile::next(measuredPrepare, "point_hierarchy");\n    if (buildPointHierarchy &&'),
        ('    return result;', '    obj_profile::end(measuredPrepare);\n    return result;'),
    ])
    (output / "scene_mesh_preparation.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    text = (root / "src/point_cloud.cpp").read_text(encoding="utf-8")
    text = section(text, 'std::optional<Cloud> buildCloud(', 'Selection selectDetail(', [
        ('        auto low=position(first), high=low;', '        auto measuredPoints = obj_profile::begin("point_range_bounds");\n        auto low=position(first), high=low;'),
        ('        std::vector<Key> keys(end-first);', '        obj_profile::next(measuredPoints, "point_morton_keys");\n        std::vector<Key> keys(end-first);'),
        ('        radix(keys,cancel);', '        obj_profile::next(measuredPoints, "point_radix_sort");\n        radix(keys,cancel);\n        obj_profile::next(measuredPoints, "point_sorted_gather");'),
        ('        cloud.roots.push_back(buildNode', '        obj_profile::next(measuredPoints, "point_tree_build");\n        cloud.roots.push_back(buildNode'),
        ('leafSize,proxySize,cancel));', 'leafSize,proxySize,cancel));\n        obj_profile::end(measuredPoints);'),
        ('    std::vector<uint32_t> budgets', '    const auto measuredCuts = obj_profile::begin("point_navigation_cuts");\n    std::vector<uint32_t> budgets'),
        ('    } catch (const Canceled&)', '    obj_profile::end(measuredCuts, {{"points", cloud.points.size()}, {"nodes", cloud.nodes.size()},\n        {"proxies", cloud.proxies.size()}, {"point_capacity_bytes", cloud.points.capacity()*sizeof(Point)}});\n    } catch (const Canceled&)'),
    ])
    (output / "point_cloud.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")
    text = (root / "src/surface_annotation.cpp").read_text(encoding="utf-8")
    text = section(text, 'std::shared_ptr<const MeshAnnotationCache> buildAnnotationMeshCache(', 'std::string gestureFingerprint(', [
        ('    auto cache =', '    auto measuredAnnotations = obj_profile::begin("annotation_blocks_fingerprints");\n    auto cache ='),
        ('    auto spatial =', '    obj_profile::next(measuredAnnotations, "annotation_morton_keys");\n    auto spatial ='),
        ('        stableSpatialRadixSort(keys,', '        obj_profile::next(measuredAnnotations, "annotation_radix_sort");\n        stableSpatialRadixSort(keys,'),
        ('    const auto build =', '    obj_profile::next(measuredAnnotations, "annotation_tree_build");\n    const auto build ='),
        ('    cache->spatial = std::move(spatial);', '    obj_profile::end(measuredAnnotations, {{"triangle_offsets", spatial->triangles.size()},\n        {"tree_nodes", spatial->tree.size()}, {"tree_capacity_bytes", spatial->tree.capacity()*sizeof(MeshAnnotationNode)}});\n    cache->spatial = std::move(spatial);'),
    ])
    (output / "surface_annotation.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")
    text = (root / "src/ui_state.cpp").read_text(encoding="utf-8")
    text = section(text, 'std::vector<UiGroupState> createUiGroupStates(', 'UiFileState createUiFileState(', [
        ('        group.localBounds = mesh.preparationDeferred ? mesh.bounds',
         '        auto measuredGroup = obj_profile::begin("group_render_bounds");\n        group.localBounds = mesh.preparationDeferred ? mesh.bounds'),
        ('        if (!mesh.preparationDeferred) { group.originalBounds = originalMeshBounds(',
         '        obj_profile::next(measuredGroup, "group_original_bounds");\n        if (!mesh.preparationDeferred) { group.originalBounds = originalMeshBounds('),
        ('        group.center = group.localBounds.center;', '        obj_profile::end(measuredGroup);\n        group.center = group.localBounds.center;'),
    ])
    (output / "ui_state.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
