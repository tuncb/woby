"""Install research-only picking variants in a disposable worktree."""
from pathlib import Path
import argparse
import importlib.util
import shutil


def instrument(root):
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location("fps_instrument", here.parent / "render_fps/instrument.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.instrument(root)
    for name in ("point_index.h", "adapter.h", "index_tests.cpp", "candidates.sc", "reduce.sc"):
        shutil.copyfile(here / name, root / "src" / ("hover_experiment.h" if name == "adapter.h" else name))
    source = root / "src/main.cpp"
    text = source.read_text(encoding="utf-8")
    text = text.replace('#include "render_fps_probe.h"', '#include "render_fps_probe.h"\n#include "hover_experiment.h"')
    text = text.replace('render_fps::Capture capture;', 'const bool experimentEnabled = !render_fps::environment("WOBY_RENDER_FPS_CONFIG").empty();\n        hover_experiment::Runtime hoverExperiment;\n        render_fps::Capture capture;')
    text = text.replace('render_fps::initialize(capture, ui);', 'if (experimentEnabled) { render_fps::initialize(capture, ui); }')
    text = text.replace('render_fps::prepare(capture, ui, window.get(), resetFlags);', 'if (experimentEnabled) { render_fps::prepare(capture, ui, window.get(), resetFlags); }')
    text = text.replace('const auto probeMouse = render_fps::mouse(', 'const auto probeMouse = experimentEnabled ? render_fps::mouse(')
    text = text.replace('static_cast<float>(viewport.width), static_cast<float>(viewport.height));', 'static_cast<float>(viewport.width), static_cast<float>(viewport.height)) : std::array<float, 2>{};')
    text = text.replace('const MousePosition windowMouse{probeMouse[0], probeMouse[1]};', 'const MousePosition windowMouse = experimentEnabled ? MousePosition{probeMouse[0], probeMouse[1]} : mousePositionInPixels(window.get());')
    text = text.replace('const bool cameraInteractionActive = render_fps::dragging(capture);', 'const bool cameraInteractionActive = experimentEnabled ? render_fps::dragging(capture) : (cameraInput.orbiting || cameraInput.rolling || cameraInput.panning);')
    text = text.replace('const bool hoverPickingEnabled = mouseInsideViewport', 'const bool hoverPickingEnabled = mouseInsideViewport\n                    && (experimentEnabled || !ImGui::GetIO().WantCaptureMouse)')
    start = text.index('                if (!hoverPickingEnabled) {', text.index('const bool hoverPickingEnabled'))
    end = text.index('                recordFrameStage(frameTimings, woby::FrameStage::hoverPick', start)
    original = text[start:end]
    text = text[:start] + '''                if (experimentEnabled) {
                  if (hoverPickingEnabled) {
                    hoveredVertex = hover_experiment::update(hoverExperiment, ui, runtimes, currentPickView, mouse, capture, assets);
                  }
                }
                else {
''' + original + '                }\n' + text[end:]
    text = text.replace('            render_fps::end(capture, ui, frameTimings, frameNumber);', '            if (experimentEnabled) { hover_experiment::poll(hoverExperiment, frameNumber, capture);\n            render_fps::end(capture, ui, frameTimings, frameNumber); }')
    text = text.replace('        render_fps::save(capture);', '        if (experimentEnabled) { hover_experiment::finish(hoverExperiment, capture);\n        render_fps::save(capture); }')
    text = text.replace('init.type = bgfx::RendererType::Direct3D11;\n        init.vendorId = 0x10de;', 'init.type = bgfx::RendererType::Count;\n        if (!render_fps::environment("WOBY_RENDER_FPS_CONFIG").empty()) { init.type = bgfx::RendererType::Direct3D11; init.vendorId = 0x10de; }')
    text = text.replace('bgfx::setDebug(headless ? BGFX_DEBUG_NONE : BGFX_DEBUG_TEXT);', 'bgfx::setDebug(headless ? BGFX_DEBUG_NONE : (!render_fps::environment("WOBY_RENDER_FPS_CONFIG").empty() ? BGFX_DEBUG_PROFILER : BGFX_DEBUG_TEXT));')
    # The experiment deliberately queries during orbit/pan too.
    source.write_text(text, encoding="utf-8")
    cmake = root / "CMakeLists.txt"
    text = cmake.read_text(encoding="utf-8")
    anchor = 'include(cmake/BgfxShaders.cmake)'
    additions = '\n'.join(f'''woby_compile_bgfx_shader(woby
    SOURCE "${{CMAKE_CURRENT_SOURCE_DIR}}/src/{name}.sc"
    TYPE compute OUTPUT_NAME cs_hover_{name}
    VARYING "${{CMAKE_CURRENT_SOURCE_DIR}}/shaders/varying.def.sc")''' for name in ("candidates", "reduce"))
    text = text.replace(anchor, anchor + '\n' + additions)
    text = text.replace('    add_executable(woby_tests', '    add_executable(woby_tests\n        src/index_tests.cpp')
    cmake.write_text(text, encoding="utf-8")
    probe = root / "src/render_fps_probe.h"
    text = probe.read_text(encoding="utf-8")
    text = text.replace('frames >= 20', 'frames >= config.value("warmup_frames", 20u)')
    text = text.replace('    // Identical two-second path', '    if (config.value("validate", false)) { elapsed = static_cast<double>(frames)*0.2; }\n    // Identical two-second path')
    probe.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worktree", type=Path)
    instrument(parser.parse_args().worktree.resolve())
