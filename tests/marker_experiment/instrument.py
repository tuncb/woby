"""Install marker-ID research in a clean disposable checkout. No production edits."""
from pathlib import Path
import argparse
import importlib.util
import shutil

HERE = Path(__file__).resolve().parent


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError(f"Expected {count} occurrences: {old!r}; found {text.count(old)}")
    return text.replace(old, new)


def instrument(root):
    spec = importlib.util.spec_from_file_location("hover_instrument", HERE.parent / "hover_experiment/instrument.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.instrument(root)
    for name in ("marker_logic.h", "marker_hooks.h", "marker_logic_tests.cpp", "adapter.h"):
        shutil.copyfile(HERE / name, root / "src" / ("marker_experiment.h" if name == "adapter.h" else name))
    shader_dir = root / "src/marker_shaders"
    shader_dir.mkdir(exist_ok=True)
    # A full register prevents HLSL packing smooth UVs beside a flat ID.
    varying = (root / "shaders/varying.def.sc").read_text() + "\nflat vec4 v_markerId : TEXCOORD2;\n"
    (shader_dir / "varying.def.sc").write_text(varying)
    shaders = []

    def shader(name, kind, text):
        (shader_dir / (name + ".sc")).write_text(text)
        shaders.append((name, kind))

    point_vs = (root / "shaders/point_sprite.vert.sc").read_text()
    point_vs = replace(point_vs, "$output v_texcoord0", "$output v_texcoord0, v_markerId")
    point_vs = replace(point_vs, "    v_texcoord0 = corner;", "    v_texcoord0 = corner;\n    uint markerId=pointBase+uint(gl_InstanceID)+1u;\n    v_markerId=vec4(float(markerId&65535u),float(markerId>>16u),0.0,0.0);")
    shader("vs_marker_point", "vertex", point_vs)
    point_fs = (root / "shaders/point_sprite.frag.sc").read_text()
    point_fs = replace(point_fs, "$input v_texcoord0", "$input v_texcoord0, v_markerId")
    point_fs = replace(point_fs, "uniform vec4 u_color;", "uniform vec4 u_color;\nuniform vec4 u_markerTag;")
    point_fs = replace(point_fs, "if (length(v_texcoord0) > 0.5)", "if (length(v_texcoord0) > 0.5 || u_color.a <= 0.000001)")
    point_fs = replace(point_fs, "gl_FragColor = u_color;", "gl_FragData[0] = u_color;\n    gl_FragData[1] = vec4(v_markerId.xy,gl_FragCoord.z,u_markerTag.x);")
    shader("fs_marker_point", "fragment", point_fs)
    compact_fs = replace(point_fs, "gl_FragData[1] = vec4(v_markerId.xy,gl_FragCoord.z,u_markerTag.x);", """uint id=uint(v_markerId.x)|(uint(v_markerId.y)<<16u);
    gl_FragData[1] = vec4(float(id&255u),float((id>>8u)&255u),float((id>>16u)&255u),float(id>>24u))/255.0;""")
    shader("fs_marker_point_compact", "fragment", compact_fs)
    for source, output in (("mesh", "mesh"), ("color", "line")):
        text = (root / f"shaders/{source}.frag.sc").read_text().replace("gl_FragColor", "gl_FragData[0]")
        text = replace(text, "void main()\n{", "void main()\n{\n    if (u_color.a <= 0.000001) { discard; }\n    gl_FragData[1] = vec4_splat(0.0);")
        shader("fs_marker_" + output, "fragment", text)
    for msaa in (0, 1):
        suffix = "msaa" if msaa else "single"
        for compact in (0,1):
            name = suffix + ("_compact" if compact else "")
            defines = f"#define MARKER_MSAA {msaa}\n#define MARKER_COMPACT {compact}\n"
            shader("cs_marker_lookup_" + name, "compute", defines + (HERE / "lookup.sc").read_text())
            text = (HERE / "highlight.frag.sc").read_text()
            text = text.replace("#include", defines + "#include", 1)
            shader("fs_marker_highlight_" + name, "fragment", text)
    for highlight in (0, 1):
        text = (HERE / "screen.vert.sc").read_text().replace("#include", f"#define MARKER_HIGHLIGHT {highlight}\n#include", 1)
        shader("vs_marker_" + ("highlight" if highlight else "screen"), "vertex", text)
    shader("fs_marker_composite", "fragment", (HERE / "composite.frag.sc").read_text())

    cmake = root / "CMakeLists.txt"
    text = cmake.read_text()
    additions = "\n".join(f'''woby_compile_bgfx_shader(woby
    SOURCE "${{CMAKE_CURRENT_SOURCE_DIR}}/src/marker_shaders/{name}.sc"
    TYPE {kind} OUTPUT_NAME {name}
    VARYING "${{CMAKE_CURRENT_SOURCE_DIR}}/src/marker_shaders/varying.def.sc")''' for name, kind in shaders)
    text = replace(text, "include(cmake/BgfxShaders.cmake)", "include(cmake/BgfxShaders.cmake)\n" + additions)
    text = replace(text, "    add_executable(woby_tests", "    add_executable(woby_tests\n        src/marker_logic_tests.cpp")
    cmake.write_text(text)

    source = root / "src/scene_renderer.cpp"
    text = source.read_text()
    text = replace(text, '#include "scene_renderer.h"', '#include "scene_renderer.h"\n#include "marker_hooks.h"')
    text = text.replace("bgfx::setState(renderState(", "marker_experiment::setDrawState(renderState(")
    text = replace(text, "    const auto pointParams = pointSpriteParameters", "    marker_experiment::recordDraw(model,indexOffset,indexCount,pointSize);\n    const auto pointParams = pointSpriteParameters")
    source.write_text(text)

    probe = root / "src/render_fps_probe.h"
    text = probe.read_text()
    text = replace(text, "    base = ui.camera;", """    if (config.value("fixture",false)) {
        woby::setSceneUpAxis(ui,woby::SceneUpAxis::y);
        woby::lookAtUiCamera(ui,{0,0,4},{0,0,0});
    }
    base = ui.camera;""")
    text = replace(text, "    if (s.value(\"offscreen\", false)) {", """    if (config.value("fixture",false)) {
        for (auto& file:ui.files) {
            const float z=s.value("translate",false) ? .1f*static_cast<float>(frames) : 0.0f;
            woby::setFileTranslation(file.fileSettings,{0,0,z});
            const int hidden=s.value("hide_group",-1);
            if (hidden>=0 && static_cast<size_t>(hidden)<file.groupSettings.size()) {
                woby::setGroupVisible(file.groupSettings[static_cast<size_t>(hidden)],false);
            }
        }
    }
    if (s.value("offscreen", false)) {""")
    text = replace(text, "if (frames == 3 && config.value(\"captures\", false))", "if ((frames == 3 || frames == 9) && config.value(\"captures\", false))")
    text = replace(text, '+ ".tga";', '+ "-frame" + std::to_string(frames) + ".tga";')
    probe.write_text(text)

    source = root / "src/main.cpp"
    text = source.read_text()
    text = replace(text, '#include "hover_experiment.h"', '#include "hover_experiment.h"\n#include "marker_experiment.h"')
    text = replace(text, 'constexpr bgfx::ViewId helperView = 2;', 'constexpr bgfx::ViewId helperView = 5;')
    text = replace(text, "hover_experiment::Runtime hoverExperiment;", "hover_experiment::Runtime hoverExperiment;\n        marker_experiment::Runtime markerExperiment;")
    text = replace(text, "                if (experimentEnabled) {\n                  if (hoverPickingEnabled)", "                if (experimentEnabled) {\n                  marker_experiment::begin(markerExperiment,currentPickView,mouse,capture,assets);\n                  if (marker_experiment::isMode(markerExperiment.mode)) { hoveredVertex=markerExperiment.last; }\n                  else if (hoverPickingEnabled)")
    anchor = """                        meshProgram,
                        colorProgram,
                        pointSpriteProgram,
                        colorUniform,
                        pointParamsUniform,
                        sceneViewportWidth,
                        sceneViewportHeight);"""
    text = replace(text, anchor, anchor.replace("meshProgram,", "markerExperiment.active ? markerExperiment.mesh : meshProgram,").replace("colorProgram,", "markerExperiment.active ? markerExperiment.line : colorProgram,").replace("pointSpriteProgram,", "markerExperiment.active ? markerExperiment.point : pointSpriteProgram,"))
    text = replace(text, "                woby::submitComparisonScenes(sceneView", "                if (experimentEnabled) { marker_experiment::submit(markerExperiment,capture,viewport); }\n                woby::submitComparisonScenes(sceneView")
    text = replace(text, "hover_experiment::poll(hoverExperiment, frameNumber, capture);", "hover_experiment::poll(hoverExperiment, frameNumber, capture);\n            marker_experiment::poll(markerExperiment,frameNumber,capture,ui,runtimes);")
    text = replace(text, "hover_experiment::finish(hoverExperiment, capture);", "marker_experiment::finish(markerExperiment,capture,ui,runtimes);\n        hover_experiment::finish(hoverExperiment, capture);")
    text = replace(text, "const woby::SceneViewport& viewport, uint32_t drawableWidth)\n{\n    if (!hoveredVertex", "const woby::SceneViewport& viewport, uint32_t drawableWidth, bool async = false)\n{\n    if (!hoveredVertex")
    text = replace(text, "        const auto& local = hoveredVertex->localPosition;", '        if (async) { ImGui::TextUnformatted("Coordinates (asynchronous)"); }\n        const auto& local = hoveredVertex->localPosition;')
    text = replace(text, "drawHoveredVertexOverlay(hoveredVertex, viewport, width);", 'drawHoveredVertexOverlay(hoveredVertex, viewport, width, markerExperiment.mode=="id_async" || markerExperiment.mode=="id_immediate");')
    source.write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worktree", type=Path)
    instrument(parser.parse_args().worktree.resolve())
