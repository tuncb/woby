"""Generate instrumented copies of the current viewer; retain production behavior."""
from pathlib import Path
import sys

from generate import generate as generate_cpu, replace, section


def generate(root, output, reference=False):
    generate_cpu(root, output, reference=reference)
    (output / "profile.h").write_text('#include "e2e_trace.h"\n', encoding="utf-8")
    mapping = "experiments/obj-mapping-reference/obj_corner_mapping.h" if reference else "src/obj_corner_mapping.h"
    build = 'inline void buildFaces(' if reference else 'inline void buildGeometry('
    text = (root / mapping).read_text(encoding="utf-8")
    text = section(text, 'inline void resolveFaces(', 'inline Vertex gatherVertex(' if reference else 'inline void resolvePrimitives(', [
        ('    table.primary.resize(sourcePositions(source).size());',
         '    auto measuredResolve = obj_profile::begin("mapping_table_allocate");\n    table.primary.resize(sourcePositions(source).size());'),
        ('    geometry.indices.resize(corners);',
         '    obj_profile::next(measuredResolve, "mapping_index_initialize");\n    geometry.indices.resize(corners);'),
        ('    const bool allFiniteUvs =',
         '    obj_profile::next(measuredResolve, "mapping_id_resolution");\n    const bool allFiniteUvs ='),
        ('    }\n}\ntemplate<bool Precise>',
         '    }\n    obj_profile::end(measuredResolve, {{"corners", corners}, {"vertices", table.next}, {"seams", table.seams.size()}});\n}\ntemplate<bool Precise>'),
    ])
    worker_range = 'PrimaryPass ? "mapping_primary_worker" : "mapping_seam_worker"'
    if not reference:
        worker_range = 'Cloud ? "mapping_cloud_worker" : ' + worker_range
    text = section(text, 'inline size_t gatherPass(', build, [
        ('    const auto work = [&](size_t worker) {',
         '    const auto work = [&](size_t worker) {\n        const obj_profile::Range measuredWorker(' + worker_range + ');'),
    ])
    table = '    Table table; metrics.staged = true;' if reference else '    Table table;'
    text = section(text, build, '} // namespace woby::obj_mapping', [
        (table, '    auto measuredMapping = obj_profile::begin("mapping_resolve_total");\n' + table),
        ('    metrics.resolveMs = elapsed(start);',
         '    metrics.resolveMs = elapsed(start);\n    obj_profile::next(measuredMapping, "mapping_parser_records_release");'),
        ('    metrics.releaseMs = elapsed(start);',
         '    metrics.releaseMs = elapsed(start);\n    obj_profile::next(measuredMapping, "mapping_vertex_allocate");'),
        ('    bool identity = table.seams.empty();',
         '    obj_profile::next(measuredMapping, "mapping_position_map");\n    bool identity = table.seams.empty();'),
        ('    metrics.positionMapMs = elapsed(start);',
         '    metrics.positionMapMs = elapsed(start);\n    obj_profile::next(measuredMapping, "mapping_primary_gather");'),
        ('    metrics.primaryGatherMs = elapsed(start); start = Clock::now();',
         '    metrics.primaryGatherMs = elapsed(start); start = Clock::now();\n    obj_profile::next(measuredMapping, "mapping_seam_gather");'),
        ('    metrics.gatherWorkers = std::max(metrics.gatherWorkers,seamWorkers); metrics.seamGatherMs = elapsed(start);',
         '    metrics.gatherWorkers = std::max(metrics.gatherWorkers,seamWorkers); metrics.seamGatherMs = elapsed(start);\n    obj_profile::next(measuredMapping, "mapping_lookup_release");\n    table = {};\n    obj_profile::end(measuredMapping);'),
    ])
    if not reference:
        text = replace(text, '        resolvePrimitives<Precise>(result,table,geometry,progress,corners,totalIndices);',
                       '        const obj_profile::Range measuredPrimitives("mapping_primitive_resolution");\n        resolvePrimitives<Precise>(result,table,geometry,progress,corners,totalIndices);')
    (output / "obj_corner_mapping.h").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    text = (root / "src/post_loading.cpp").read_text(encoding="utf-8")
    text = section(text, 'std::optional<PostLoadData> preparePostLoadData(', 'PostLoadIdentity postLoadIdentity(', [
        ('    PostLoadData result;',
         '    const obj_profile::Range measuredPostLoad("post_load_cpu_total");\n    auto measuredPost = obj_profile::begin("post_source_indices");\n    PostLoadData result;'),
        ('    auto originalReduction =',
         '    obj_profile::next(measuredPost, "post_original_bounds", {{"source_index_capacity_bytes", result.source ? result.source->indices.capacity()*sizeof(uint32_t) : 0}});\n    auto originalReduction ='),
        ('    result.originalBounds = boundsResult(originalReduction);',
         '    result.originalBounds = boundsResult(originalReduction);\n    obj_profile::end(measuredPost);\n    const obj_profile::Range measuredPreparation("post_parallel_preparation");'),
        ('        auto points = prepareSceneMesh(',
         '        auto measuredPoints = obj_profile::begin("post_point_preparation");\n        auto points = prepareSceneMesh('),
        ('        result.groups.reserve(points->nodeRanges.size());',
         '        obj_profile::next(measuredPoints, "post_group_bounds");\n        size_t measuredReusedGroups = 0, measuredPartialVertices = 0;\n        result.groups.reserve(points->nodeRanges.size());'),
        ('            bounds.local = mesh.bounds;',
         '            ++measuredReusedGroups;\n            bounds.local = mesh.bounds;'),
        ('            auto localReduction =',
         '            measuredPartialVertices += range.pointIndexCount;\n            auto localReduction ='),
        ('        result.points = std::move(*points);',
         '        result.points = std::move(*points);\n        obj_profile::end(measuredPoints, {{"reused_groups", measuredReusedGroups}, {"partial_group_vertices", measuredPartialVertices}});'),
        ('                else { result.annotations = buildAnnotationMeshCache(mesh, stop); }',
         '                else { const obj_profile::Range measuredAnnotations("post_annotation_cache");\n                    result.annotations = buildAnnotationMeshCache(mesh, stop); }'),
    ])
    text = replace(text, '                runtime.upload = beginGpuPointUpload(',
                   '                obj_profile::mark("post_point_upload_queued");\n                runtime.upload = beginGpuPointUpload(')
    text = replace(text, '                publishPostLoadData(state, job.identity, std::move(*job.result));',
                   '                obj_profile::mark("post_point_upload_complete");\n                const auto measuredPublish = obj_profile::begin("post_result_publish");\n                publishPostLoadData(state, job.identity, std::move(*job.result));\n                obj_profile::end(measuredPublish);\n                obj_profile::mark("all_tools_ready");')
    (output / "post_loading.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    text = (root / "src/graphics_upload.cpp").read_text(encoding="utf-8")
    text = replace(text, '    const auto start = std::chrono::steady_clock::now();',
                   '    const obj_profile::Range measuredUpload("gpu_upload_worker_total");\n    auto measuredUploadStage = obj_profile::begin("gpu_upload_source_prepare");\n    const auto start = std::chrono::steady_clock::now();')
    text = replace(text, '        const auto before = runtime.uploader ? runtime.uploader->stats() : gpu::UploadQueueStats{};',
                   '        obj_profile::next(measuredUploadStage, "gpu_buffer_allocate_and_staging");\n        const auto before = runtime.uploader ? runtime.uploader->stats() : gpu::UploadQueueStats{};')
    text = replace(text, '        if (runtime.uploader) {\n            job.completion = runtime.uploader->flush();',
                   '        obj_profile::next(measuredUploadStage, "gpu_upload_final_fence_wait");\n        if (runtime.uploader) {\n            job.completion = runtime.uploader->flush();')
    text = replace(text, '    job.stats.uploadedBytes = job.uploadedBytes.load(std::memory_order_relaxed);',
                   '    obj_profile::end(measuredUploadStage, {{"uploaded_bytes", job.uploadedBytes.load(std::memory_order_relaxed)},\n        {"submissions", job.stats.submissions}, {"waits", job.stats.waits}, {"staging_bytes", job.stats.stagingBytes}});\n    job.stats.uploadedBytes = job.uploadedBytes.load(std::memory_order_relaxed);')
    (output / "graphics_upload.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    text = (root / "src/model_load.cpp").read_text(encoding="utf-8")
    text = replace(text, '    ImportedModel result;',
                   '    const obj_profile::Range measuredImport("cpu_import");\n    ImportedModel result;')
    (output / "model_load.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")

    text = (root / "src/main.cpp").read_text(encoding="utf-8")
    text = replace(text, '        auto completeAppend = [&]() {',
                   '        uint64_t measuredFirstFrame = 0;\n        bool measuredFirstComplete = false;\n        auto completeAppend = [&]() {')
    text = replace(text, '            if (!historyRestore.active) { woby::updatePostLoading(',
                   '            if (measuredFirstFrame && !measuredFirstComplete && woby::graphics::completedFrameToken() >= measuredFirstFrame) {\n                obj_profile::mark("first_scene_frame_gpu_complete", {{"frame_token", measuredFirstFrame}});\n                measuredFirstComplete = true;\n            }\n            if (!historyRestore.active) { woby::updatePostLoading(')
    text = replace(text, '            woby::notePostLoadFrame(postLoading, ui, woby::graphics::submittedFrameToken());',
                   '            woby::notePostLoadFrame(postLoading, ui, woby::graphics::submittedFrameToken());\n            if (!ui.files.empty() && !measuredFirstFrame) {\n                measuredFirstFrame = woby::graphics::submittedFrameToken();\n                obj_profile::mark("first_scene_frame_submitted", {{"frame_token", measuredFirstFrame}, {"frame_index", frameIndex}});\n            }')
    text = replace(text, '                                automationAppend = AutomationAppendRuntime{command->id, ui.files.size(), {}, false};',
                   '                                obj_profile::mark("model_add_requested");\n                                automationAppend = AutomationAppendRuntime{command->id, ui.files.size(), {}, false};')
    text = section(text, '        auto completeAppend = [&]() {', '        DragDropState dragDropState;', [
        ('            automationAppend.reset();',
         '            obj_profile::mark("model_add_rpc_complete");\n            automationAppend.reset();'),
    ])
    (output / "main.cpp").write_text('#include "profile.h"\n' + text, encoding="utf-8")


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
