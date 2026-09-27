"""Build a disposable instrumented full viewer using the local VS preset.

Production .cpp files are never edited. CMakeLists is restored byte-for-byte
in finally; the probe has a distinct executable name. Run with no other builds.
"""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/first-display-probe"


def replace(text, old, new):
    assert text.count(old) == 1, (old, text.count(old))
    return text.replace(old, new)


def generate():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["uv", "run", str(ROOT / "tests/vertex_mapping/generate.py"),
                    str(ROOT), str(OUT)], check=True)
    shutil.copyfile(OUT / "obj_adaptive.cpp", OUT / "obj_mesh.cpp")
    text = (ROOT / "src/ui_state.cpp").read_text()
    text = '#include "mapping_probe.h"\n' + text
    text = replace(text, "    prepareAnnotationMeshCache(file.mesh);", '    mapping_probe::start();\n    prepareAnnotationMeshCache(file.mesh);\n    mapping_probe::mark("annotation_cache_ms");')
    text = replace(text, "    file.groupSettings = createUiGroupStates(file.mesh, firstColorIndex);", '    file.groupSettings = createUiGroupStates(file.mesh, firstColorIndex);\n    mapping_probe::mark("group_state_ms");')
    (OUT / "ui_state.cpp").write_text(text)

    text = '#include "mapping_probe.h"\n' + (ROOT / "src/scene_renderer.cpp").read_text()
    text = replace(text, "    GpuMesh gpuMesh;", "    mapping_probe::start();\n    GpuMesh gpuMesh;")
    text = replace(text, "    gpuMesh.nodeRanges.reserve(mesh.nodes.size());", '    mapping_probe::mark("gpu_index_validate_ms");\n    gpuMesh.nodeRanges.reserve(mesh.nodes.size());')
    text = replace(text, "    std::vector<size_t> vertexGroups(mesh.vertices.size(), mesh.nodes.size());", '    std::vector<size_t> vertexGroups(mesh.vertices.size(), mesh.nodes.size());\n    mapping_probe::mark("point_allocate_ms");')
    text = replace(text, "    try {\n        gpuMesh.vertexBuffer", '    mapping_probe::mark("point_ranges_ms");\n    try {\n        const auto* vertexMemory = bgfx::copy(mesh.vertices.data(), vertexBytes);\n        mapping_probe::mark("vertex_staging_ms");\n        gpuMesh.vertexBuffer')
    text = replace(text, "bgfx::copy(mesh.vertices.data(), vertexBytes),", "vertexMemory,")
    text = replace(text, "        gpuMesh.triangleIndexBuffer = bgfx::createIndexBuffer(", '        mapping_probe::mark("vertex_enqueue_ms");\n        const auto* indexMemory = bgfx::copy(mesh.indices.data(), indexBytes);\n        mapping_probe::mark("index_staging_ms");\n        gpuMesh.triangleIndexBuffer = bgfx::createIndexBuffer(')
    text = replace(text, "bgfx::copy(mesh.indices.data(), indexBytes),", "indexMemory,")
    text = replace(text, "        prepareGpuMeshFeatures(gpuMesh, mesh, features);", '        mapping_probe::mark("index_enqueue_ms");\n        prepareGpuMeshFeatures(gpuMesh, mesh, features);\n        mapping_probe::mark("optional_gpu_features_ms");')
    (OUT / "scene_renderer.cpp").write_text(text)

    text = '#include "probe.h"\n' + (ROOT / "src/main.cpp").read_text()
    text = replace(text, "        const auto startupStart = woby::PerformanceClock::now();", "        const auto startupStart = woby::PerformanceClock::now();\n        display_probe::startupStart = startupStart;")
    text = replace(text, "        bgfx::Init init;", "        bgfx::renderFrame(); // Controlled, synchronous render thread.\n        bgfx::Init init;")
    text = replace(text, "init.type = bgfx::RendererType::Count;", "init.type = bgfx::RendererType::Direct3D11;")
    text = replace(text, "    LoadedModelFileWithRuntime loaded;", "    display_probe::loadStart = totalStart;\n    LoadedModelFileWithRuntime loaded;")
    text = replace(text, "        const double parseMilliseconds = elapsedMilliseconds(parseStart);", '        const double parseMilliseconds = elapsedMilliseconds(parseStart);\n        display_probe::totals["cpu_load_ms"] = parseMilliseconds;')
    text = replace(text, "        const auto gpuStart = woby::PerformanceClock::now();", '        display_probe::totals["ui_state_ms"] = elapsedMilliseconds(parseStart) - parseMilliseconds;\n        const auto gpuStart = woby::PerformanceClock::now();')
    text = replace(text, "        const double gpuMilliseconds = elapsedMilliseconds(gpuStart);", '''        const double gpuMilliseconds = elapsedMilliseconds(gpuStart);
        display_probe::totals["gpu_prepare_ms"] = gpuMilliseconds;
        display_probe::totals["model_file_ms"] = elapsedMilliseconds(totalStart);
        display_probe::totals["vertices"] = loaded.file.mesh.vertices.size();
        display_probe::totals["triangles"] = loaded.file.mesh.indices.size()/3;
        display_probe::totals["groups"] = loaded.file.groupSettings.size();
        display_probe::totals["vertex_bytes"] = loaded.file.mesh.vertices.size()*sizeof(woby::Vertex);
        display_probe::totals["index_bytes"] = loaded.file.mesh.indices.size()*sizeof(uint32_t);
        display_probe::totals["point_entries"] = loaded.runtime.gpuMesh.pointVertexIndices.size();
''')
    text = replace(text, "        while (running) {", '''        display_probe::totals["load_to_loop_ms"] = mapping_probe::elapsed(display_probe::loadStart);
        display_probe::totals["vendor_id"] = bgfx::getCaps()->vendorId;
        display_probe::totals["device_id"] = bgfx::getCaps()->deviceId;
        while (running) {''')
    text = replace(text, "            const uint32_t frameNumber = bgfx::frame();", '''            const auto probeFrameStart = mapping_probe::Clock::now();
            if (display_probe::frames == 1) {
                bgfx::requestScreenShot(BGFX_INVALID_HANDLE, (display_probe::outputPath() + ".tga").c_str());
            }
            const uint32_t frameNumber = bgfx::frame();
            if (display_probe::frames == 0) {
                display_probe::totals["first_frame_execute_ms"] = mapping_probe::elapsed(probeFrameStart);
                display_probe::totals["first_gpu_wait_ms"] = display_probe::finishGpu();
                display_probe::totals["load_to_first_complete_ms"] = mapping_probe::elapsed(display_probe::loadStart);
                display_probe::totals["startup_to_first_complete_ms"] = mapping_probe::elapsed(display_probe::startupStart);
                display_probe::totals["first_frame_cpu_ms"] = woby::millisecondsBetween(frameStart, probeFrameStart);
                display_probe::totals["width"] = width;
                display_probe::totals["height"] = height;
                display_probe::save();
            }
            if (++display_probe::frames >= 3) { running = false; }
''')
    (OUT / "main.cpp").write_text(text)


def main():
    generate()
    cmake = ROOT / "CMakeLists.txt"
    original = cmake.read_bytes()
    patch = '''
# Disposable first-display research target source substitution.
get_target_property(probe_sources woby SOURCES)
'''
    for name in ("main", "obj_mesh", "model_mesh", "ui_state", "scene_renderer"):
        patch += f'list(REMOVE_ITEM probe_sources src/{name}.cpp)\nlist(APPEND probe_sources "{OUT.as_posix()}/{name}.cpp")\n'
    patch += f'''set_property(TARGET woby PROPERTY SOURCES "${{probe_sources}}")
set_target_properties(woby PROPERTIES OUTPUT_NAME woby-first-display)
target_include_directories(woby PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/src" "${{CMAKE_CURRENT_SOURCE_DIR}}/tests/first_display" "${{CMAKE_CURRENT_SOURCE_DIR}}/tests/vertex_mapping")
target_link_libraries(woby PRIVATE d3d11)
'''
    try:
        cmake.write_bytes(original + patch.encode())
        subprocess.run(["cmake", "--preset", "vs2026-vcpkg"], cwd=ROOT, check=True)
        subprocess.run(["cmake", "--build", "--preset", "vs2026-vcpkg", "--config", "Release", "--target", "woby"], cwd=ROOT, check=True)
    finally:
        cmake.write_bytes(original)
        subprocess.run(["cmake", "--preset", "vs2026-vcpkg"], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
