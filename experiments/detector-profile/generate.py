"""Add coarse timing scopes to research copies; fail if a source anchor changes."""
import hashlib
import json
from pathlib import Path
import re
import sys

FUNCTIONS = {
    "mesh_comparison": ["meshTriangles", "buildTree", "compareSurface", "inspectTriangles",
                        "prepareAnalysisGeometry", "inspectSurfaceTopology", "prepareTopologyInspectionGeometry",
                        "prepareComparisonDiagnosticDisplay"],
    "mesh_topology": ["buildAnalysisGeometry", "buildTopologyIncidence", "inspectTopologyFindings",
                      "inspectVertexLinks", "inspectBoundaries", "inspectFinPatches"],
    "mesh_intersections": [],
    "mesh_duplicates": ["inspectDuplicates"],
    "mesh_degenerates": ["inspectDegenerates"],
    "surface_mesh_quality": ["inspectSurfaceMeshQuality", "statistics", "surfaceQualityDistributions", "surfaceQualityDisplayFaces"],
    "difference_mesh": ["read", "validate", "solid", "publish", "output", "closeSurfaces"],
}


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one anchor: {old[:100]!r}; found {text.count(old)}")
    return text.replace(old, new)


def generate(root, output):
    output.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for stem, functions in FUNCTIONS.items():
        source = root / "src" / (stem + ".cpp")
        text = source.read_text(encoding="utf-8")
        manifest[stem] = {"source_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
        for name in functions:
            pattern = rf"(?m)^[\w:<>, &*]+\b{re.escape(name)}\([^;]*?\)\s*\n\{{"
            matches = list(re.finditer(pattern, text))
            if len(matches) != 1:
                raise ValueError(f"Function anchor changed: {stem}:{name} ({len(matches)})")
            match = matches[0]
            text = text[:match.end()] + f'\n    detector_profile::Scope profileScope("{stem}.{name}");' + text[match.end():]
        if stem == "mesh_comparison":
            text = once(text, "    std::vector<uint32_t>().swap(tree.order);\n    return tree;",
                '    std::vector<uint32_t>().swap(tree.order);\n    profileScope.data = {{"triangles",tree.triangles.size()},{"nodes",tree.nodes.size()},\n        {"triangle_capacity_bytes",tree.triangles.capacity()*sizeof(tree.triangles.front())},\n        {"node_capacity_bytes",tree.nodes.capacity()*sizeof(tree.nodes.front())}};\n    return tree;')
            text = once(text, "    parallelAnalysisBatches(triangleCount, 128, stop,", "    auto queryPhase = obj_profile::begin(\"distance.queries\");\n    parallelAnalysisBatches(triangleCount, 128, stop,")
            text = once(text, "    // Fixed-order reductions keep weighted statistics reproducible regardless", "    obj_profile::end(queryPhase);\n    detector_profile::Scope reduceScope(\"distance.statistics\");\n    // Fixed-order reductions keep weighted statistics reproducible regardless")
        if stem == "mesh_intersections":
            text = once(text,"    MeshIntersections result; result.settings = settings; result.mode = topology.mode;",
                '    detector_profile::Scope profileScope("mesh_intersections.inspectIntersections");\n    MeshIntersections result; result.settings = settings; result.mode = topology.mode;')
            text = once(text, "        Tree tree;", "        detector_profile::Scope sourceScope(\"intersections.source\");\n        auto buildPhase = obj_profile::begin(\"intersections.bvh\");\n        Tree tree;")
            text = once(text, "        std::vector<size_t> queue;", '        obj_profile::end(buildPhase,{{"faces",source.faces.size()},\n            {"box_capacity_bytes",tree.boxes.capacity()*sizeof(tree.boxes.front())},\n            {"order_capacity_bytes",tree.order.capacity()*sizeof(tree.order.front())},\n            {"node_capacity_bytes",tree.nodes.capacity()*sizeof(tree.nodes.front())}});\n        detector_profile::Scope traversalScope("intersections.traversal");\n        std::vector<size_t> queue;')
        if stem == "surface_mesh_quality":
            text = once(text, "    SurfaceMeshQuality result;", "    auto qualityPhase = obj_profile::begin(\"quality.metrics_and_weld\");\n    SurfaceMeshQuality result;")
            text = once(text, "    size_t comparisons = 0;", "    obj_profile::next(qualityPhase, \"quality.edges_sort_reduce\");\n    size_t comparisons = 0;")
            text = once(text, "    std::vector<std::pair<double, double>> sizes;", "    obj_profile::next(qualityPhase, \"quality.statistics\");\n    std::vector<std::pair<double, double>> sizes;")
            text = once(text, "    return result;\n}\n\nstd::array<QualityDistribution", "    obj_profile::end(qualityPhase);\n    return result;\n}\n\nstd::array<QualityDistribution")
        text = '#include "scope.h"\n' + text
        if stem == "mesh_intersections":
            counted = '#include "predicate_counts.h"\n' + text
            counted = once(counted,"    MeshIntersections result; result.settings = settings; result.mode = topology.mode;",
                "    detector_profile::predicates = {};\n    MeshIntersections result; result.settings = settings; result.mode = topology.mode;")
            counted = once(counted,"    if (roundToNearest && definitelyDisjoint(aPoints, bPoints, aIds, bIds)) { return false; }",
                "    ++detector_profile::predicates.calls;\n    if (roundToNearest && definitelyDisjoint(aPoints, bPoints, aIds, bIds)) { ++detector_profile::predicates.fastSeparated; return false; }")
            counted = once(counted,"    if (const auto result = adaptiveTrianglesSelfIntersect(aPoints, bPoints, aIds, bIds)) { return *result; }",
                "    if (const auto result = adaptiveTrianglesSelfIntersect(aPoints, bPoints, aIds, bIds)) {\n        if (*result) { ++detector_profile::predicates.adaptiveTrue; } else { ++detector_profile::predicates.adaptiveFalse; }\n        return *result;\n    }\n    ++detector_profile::predicates.rational;")
            counted = once(counted,"                if (!overlaps(tree.boxes[a], node.box)) { continue; }",
                "                ++detector_profile::predicates.nodeBoxes;\n                if (!overlaps(tree.boxes[a], node.box)) { continue; }")
            counted = once(counted,"                    if (b <= a || !overlaps(tree.boxes[a], tree.boxes[b])) { continue; }",
                "                    if (b <= a) { continue; }\n                    ++detector_profile::predicates.faceBoxes;\n                    if (!overlaps(tree.boxes[a], tree.boxes[b])) { continue; }")
            counted = once(counted,"    result.affectedFaces = affected.size();\n    return result;",
                "    result.affectedFaces = affected.size();\n    profileScope.data = detector_profile::predicateData();\n    return result;")
            (output / "mesh_intersections_counted.cpp").write_text(counted,encoding="utf-8")
            manifest[stem]["counted_sha256"] = hashlib.sha256((output / "mesh_intersections_counted.cpp").read_bytes()).hexdigest()
        if stem == "surface_mesh_quality":
            tokenref = once(text,"void canceled(std::stop_token stop)","void canceled(const std::stop_token& stop)")
            (output / "surface_mesh_quality_tokenref.cpp").write_text(tokenref,encoding="utf-8")
            manifest[stem]["tokenref_sha256"] = hashlib.sha256((output / "surface_mesh_quality_tokenref.cpp").read_bytes()).hexdigest()
        if stem == "mesh_comparison":
            max3 = once(text,
                "std::max({lengthSquared(sub(t[1], t[0])), lengthSquared(sub(t[2], t[0])), lengthSquared(sub(t[2], t[1]))})",
                "std::max(std::max(lengthSquared(sub(t[1], t[0])), lengthSquared(sub(t[2], t[0]))), lengthSquared(sub(t[2], t[1])))")
            (output / "mesh_comparison_max3.cpp").write_text(max3,encoding="utf-8")
            manifest[stem]["max3_sha256"] = hashlib.sha256((output / "mesh_comparison_max3.cpp").read_bytes()).hexdigest()
            counted = '#include "query_counts.h"\n' + text
            counted = once(counted, "    best = triangleSquared(p, tree.triangles[hint]);",
                "    ++detector_profile::queryLocal.queries;\n    const auto trianglesBefore = detector_profile::queryLocal.triangles;\n    ++detector_profile::queryLocal.triangles;\n    best = triangleSquared(p, tree.triangles[hint]);\n    detector_profile::queryLocal.zeroHints += best == 0;")
            counted = once(counted, "        if (visit.count) {", "        if (visit.count) {\n            detector_profile::queryLocal.triangles += visit.count;")
            counted = once(counted, "if (distance < best) { best = distance; hint = static_cast<uint32_t>(i); }",
                "if (distance < best) { ++detector_profile::queryLocal.updates; best = distance; hint = static_cast<uint32_t>(i); }")
            counted = once(counted, "        const auto& node = tree.nodes[visit.child];", "        ++detector_profile::queryLocal.nodes;\n        const auto& node = tree.nodes[visit.child];")
            counted = once(counted, "    }\n}\n\nstd::vector<DiagnosticEdge> duplicateBounds", "    }\n    detector_profile::recordQuery(visits,detector_profile::queryLocal.triangles-trianglesBefore);\n}\n\nstd::vector<DiagnosticEdge> duplicateBounds")
            counted = once(counted, '    auto queryPhase = obj_profile::begin("distance.queries");',
                '    detector_profile::resetQueries();\n    auto queryPhase = obj_profile::begin("distance.queries");')
            counted = once(counted, "    parallelAnalysisBatches(triangleCount, 128, stop, [&](size_t begin, size_t end) {",
                "    parallelAnalysisBatches(triangleCount, 128, stop, [&](size_t begin, size_t end) {\n        if ((begin / 128) % detector_profile::queryStride != 0) { return; }\n        detector_profile::queryLocal = {};")
            counted = once(counted, "    });\n    obj_profile::end(queryPhase);",
                "        detector_profile::mergeQueries();\n    });\n    obj_profile::end(queryPhase,detector_profile::queryData());\n    if (detector_profile::queryStride > 1) { return result; }")
            (output / "mesh_comparison_counted.cpp").write_text(counted,encoding="utf-8")
            manifest[stem]["counted_sha256"] = hashlib.sha256((output / "mesh_comparison_counted.cpp").read_bytes()).hexdigest()
        destination = output / source.name
        destination.write_text(text, encoding="utf-8")
        manifest[stem]["generated_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
