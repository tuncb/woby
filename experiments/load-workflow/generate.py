"""Instrument copies of production sources; exact anchors fail on source drift.

Behavior controls are an experiment-only loader selector, a position-copy
ablation, and optional foreground pacing while the window remains hidden.
The copied position pool remains alive until mesh construction returns.
"""
import argparse
from pathlib import Path


def replace(text, old, new):
    count = text.count(old)
    if count != 1:
        raise ValueError(f"Expected one anchor, found {count}: {old[:120]!r}")
    return text.replace(old, new, 1)


def section(text, first, last, changes):
    begin = text.index(first)
    end = text.index(last, begin)
    part = text[begin:end]
    for old, new in changes:
        part = replace(part, old, new)
    return text[:begin] + part + text[end:]


def instrument(name, text):
    if name == 'graphics':
        return replace(text, '    const bool inactive = c.window &&',
            '    const bool inactive = !SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "WOBY_WORKFLOW_FOREGROUND") && c.window &&')
    if name == "obj_mesh":
        text = section(text, "Mesh buildObjMesh(", "} // namespace", [
            ('    const auto throwLoadError', '    auto measured = workflow::begin("mesh_validate");\n    const auto throwLoadError'),
            ('    std::vector<Coordinate> sourcePoints;', '    workflow::next(measured, "source_adoption", {{"positions", counts.sourcePositions}});\n    std::vector<Coordinate> sourcePoints;'),
            ('if constexpr (Prototype) { sourcePoints = std::move(buffers->positions); }',
             'if constexpr (Prototype) {\n        if (workflow::variant() == "prototype_copy") { sourcePoints = buffers->positions; }\n        else { sourcePoints = std::move(buffers->positions); }\n    }'),
            ('    const auto origin = coordinateOrigin(sourcePoints);',
             '    workflow::next(measured, "coordinate_localization", {{"position_bytes", sourcePoints.size() * sizeof(Coordinate)}, {"copied", !Prototype || workflow::variant() == "prototype_copy"}});\n    const auto origin = coordinateOrigin(sourcePoints);'),
            ('    reportModelLoadProgress(progress, ModelLoadStage::triangulating);',
             '    workflow::next(measured, "triangulation");\n    reportModelLoadProgress(progress, ModelLoadStage::triangulating);'),
            ('    const auto& attrib = result.attributes;', '    workflow::next(measured, "mesh_mapping");\n    const auto& attrib = result.attributes;'),
            ('    if (!empty(mesh)) { finalizeMesh(mesh, true, progress); }',
             '    workflow::end(measured);\n    if (!empty(mesh)) { finalizeMesh(mesh, true, progress); }'),
            ('        appendFreeformGeometry(mesh, std::move(freeformPatches), progress);',
             '        const auto freeformTimer = workflow::begin("freeform_tessellation_append");\n        appendFreeformGeometry(mesh, std::move(freeformPatches), progress);\n        workflow::end(freeformTimer);'),
            ('    return mesh;', '    workflow::mark("mesh_ready", workflow::meshInfo(mesh));\n    return mesh;'),
        ])
        text = section(text, 'Mesh loadObjMeshLegacy(', 'Mesh loadObjMeshTextLegacy(', [
            ('    reportModelLoadProgress', '    workflow::setPath(path.string());\n    auto measured = workflow::begin("capacity_preflight");\n    reportModelLoadProgress'),
            ('    auto result = parseObj(path);', '    workflow::next(measured, "parse");\n    auto result = parseObj(path);\n    workflow::end(measured);'),
            ('        auto input = readObjFreeform(path, progress);', '        measured = workflow::begin("legacy_freeform_reparse");\n        auto input = readObjFreeform(path, progress);'),
            ('        freeformPatches = std::move(input.patches);', '        freeformPatches = std::move(input.patches);\n        workflow::end(measured);'),
        ])
        text = section(text, 'Mesh buildObjPrototype(', '} // namespace', [
            ('    const auto start = Clock::now();', '    auto measured = workflow::begin("parse");\n    const auto start = Clock::now();'),
            ('    const auto parsedAt = Clock::now();', '    const auto parsedAt = Clock::now();\n    workflow::next(measured, "freeform_resolve", {{"parsed_chunk_bytes", parsed.stats.parsed_chunk_bytes}, {"input_bytes", parsed.stats.input_bytes}, {"chunks", parsed.stats.chunks}, {"workers", parsed.stats.workers}, {"avoided_position_copy_bytes", parsed.stats.position_copy_bytes_avoided}});'),
            ('    const auto resolvedAt = Clock::now();', '    const auto resolvedAt = Clock::now();\n    workflow::end(measured);'),
        ])
        text = section(text, 'Mesh loadObjMeshPrototype(', 'Mesh loadObjMeshTextPrototype(', [
            ('    preflightObjCapacity(path, progress);', '    workflow::setPath(path.string());\n    const auto measured = workflow::begin("capacity_preflight");\n    preflightObjCapacity(path, progress);\n    workflow::end(measured);'),
        ])
        text = section(text, 'Mesh loadObjMesh(const ', 'Mesh loadObjMeshText(', [
            ('#if defined(WOBY_RAPIDOBJ_PROTOTYPE)\n    return loadObjMeshPrototype(path, progress);\n#else\n    return loadObjMeshLegacy(path, progress);\n#endif',
             '    if (workflow::variant() == "legacy") { return loadObjMeshLegacy(path, progress); }\n    return loadObjMeshPrototype(path, progress);'),
        ])
    elif name == 'model_mesh':
        text = section(text, 'void finalizeMesh(', '} // namespace woby', [
            ('    renewMeshContentRevision(mesh);', '    auto measured = workflow::begin("normals");\n    renewMeshContentRevision(mesh);'),
            ('    reportModelLoadProgress(progress, ModelLoadStage::bounds);', '    workflow::next(measured, "mesh_bounds");\n    reportModelLoadProgress(progress, ModelLoadStage::bounds);'),
            ('    mesh.originalBounds = originalMeshBounds(mesh);', '    mesh.originalBounds = originalMeshBounds(mesh);\n    workflow::end(measured);'),
        ])
    elif name == 'background_load':
        text = section(text, 'TimedModelLoad timedModelLoad(', 'struct PrefetchedLoads', [
            ('    const auto start', '    workflow::setPath(path.string());\n    const auto measured = workflow::begin("cpu_mesh_total");\n    const auto start'),
            ('    return {std::move(imported)', '    workflow::end(measured);\n    return {std::move(imported)'),
        ])
        text = section(text, 'ModelBatchCpuLoadResult loadModelBatchCpu(', 'SceneCpuLoadResult loadSceneCpu(', [
            ('    const auto start', '    const auto measured = workflow::begin("cpu_batch_total");\n    const auto start'),
            ('            UiFileState file = createUiFileState(', '            workflow::setPath(modelPath.string());\n            UiFileState file = createUiFileState('),
            ('    return result;', '    workflow::end(measured);\n    return result;'),
        ])
    elif name == 'ui_state':
        text = section(text, 'UiFileState createUiFileState(', 'void synchronizeCoordinateFrames(', [
            ('    UiFileState file;', '    const auto measured = workflow::begin("ui_group_preparation");\n    UiFileState file;'),
            ('    return file;', '    workflow::end(measured, {{"groups", file.groupSettings.size()}});\n    return file;'),
        ])
    elif name == 'scene_mesh_preparation':
        text = section(text, 'std::optional<SceneMeshPreparation> prepareSceneMesh(', '} // namespace woby', [
            ('    const auto canceled', '    auto measured = workflow::begin("gpu_input_validation");\n    const auto canceled'),
            ('    SceneMeshPreparation result;', '    workflow::next(measured, "point_membership");\n    SceneMeshPreparation result;'),
            ('    if (buildPointHierarchy &&', '    workflow::next(measured, "point_hierarchy");\n    if (buildPointHierarchy &&'),
            ('    return result;', '    workflow::end(measured, workflow::preparationInfo(result));\n    workflow::mark("gpu_input_ready", workflow::preparationInfo(result));\n    return result;'),
        ])
    elif name == 'surface_annotation':
        text = section(text, 'std::shared_ptr<const MeshAnnotationCache> buildAnnotationMeshCache(', 'std::string gestureFingerprint(', [
            ('    auto cache =', '    auto measured = workflow::begin("annotation_blocks_fingerprints");\n    auto cache ='),
            ('    auto spatial =', '    workflow::next(measured, "annotation_spatial_index");\n    auto spatial ='),
            ('    auto snapshot =', '    workflow::next(measured, "annotation_snapshot_copy");\n    auto snapshot ='),
            ('    return cache;', '    workflow::end(measured, workflow::annotationInfo(*cache));\n    return cache;'),
        ])
    elif name == 'annotation_preparation':
        text = replace(text, '                mesh.annotationCache = job.result;',
            '                mesh.annotationCache = job.result;\n                workflow::mark("annotation_published", {{"path", file.path.string()}});')
    elif name == 'main':
        text = section(text, 'bool startAppendModelBackgroundLoad(', 'bool startAppendFolderTreeBackgroundLoad(', [
            ('    resetBackgroundLoadProgress(', '    woby::workflow::mark("load_requested", {{"files", modelPaths.size()}});\n    resetBackgroundLoadProgress('),
        ])
        text = section(text, 'void startGpuFinalize(', 'std::string appendFinalizeStatus(', [
            ('    finalize = GpuFinalizeRuntime{};', '    woby::workflow::mark("gpu_finalize_begin");\n    finalize = GpuFinalizeRuntime{};'),
        ])
        text = section(text, 'void commitGpuFinalize(', 'std::optional<std::string> processGpuFinalizeStep(', [
            ('    finalize = GpuFinalizeRuntime{};', '    woby::workflow::mark("scene_committed", {{"files", state.files.size()}});\n    finalize = GpuFinalizeRuntime{};'),
        ])
        text = replace(text, '                LoadedModelRuntime runtime;\n                runtime.requestedFeatures',
            '                woby::workflow::mark("gpu_upload", {{"path", file.path.string()}, {"bytes", finalize.upload->uploadedBytes}, {"cpu_ms", finalize.uploadMilliseconds}, {"longest_step_ms", finalize.longestUploadStepMilliseconds}});\n                LoadedModelRuntime runtime;\n                runtime.requestedFeatures')
        text = replace(text, '            const uint32_t frameNumber = woby::graphics::frame();',
            '            const uint32_t frameNumber = woby::graphics::frame();\n            woby::workflow::frameSubmitted(!ui.files.empty());')
    else:
        raise ValueError(name)
    return '#include "trace.h"\n' + text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('repository', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name in ('obj_mesh', 'model_mesh', 'background_load', 'ui_state', 'scene_mesh_preparation',
                 'surface_annotation', 'annotation_preparation', 'main', 'graphics'):
        source = (args.repository / 'src' / f'{name}.cpp').read_text(encoding='utf-8')
        instrumented = instrument(name, source)
        destination = args.output / f'{name}.cpp'
        if not destination.exists() or destination.read_text(encoding='utf-8') != instrumented:
            destination.write_text(instrumented, encoding='utf-8')


if __name__ == '__main__':
    main()
