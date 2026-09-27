"""Patch a disposable worktree, never the production checkout. Anchors fail closed."""
import argparse
from pathlib import Path
import shutil


def instrument(root):
    source = root / "src/main.cpp"
    text = source.read_text(encoding="utf-8")
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError(f"Expected one source anchor: {old!r}")
        text = text.replace(old, new)
    replace('#include "camera.h"', '#include "camera.h"\n#include "render_fps_probe.h"')
    replace('constexpr uint32_t resetFlags =', 'uint32_t resetFlags =')
    replace('init.type = bgfx::RendererType::Count;', 'init.type = bgfx::RendererType::Direct3D11;\n        init.vendorId = 0x10de;')
    replace('        while (running) {', '        render_fps::Capture capture;\n        render_fps::initialize(capture, ui);\n        while (running) {')
    replace('            auto stageStart = frameStart;', '            auto stageStart = frameStart;\n            render_fps::prepare(capture, ui, window.get(), resetFlags);')
    replace('const MousePosition windowMouse = mousePositionInPixels(window.get());', '''const auto probeMouse = render_fps::mouse(capture, static_cast<float>(viewport.x), static_cast<float>(viewport.y),
                    static_cast<float>(viewport.width), static_cast<float>(viewport.height));
                const MousePosition windowMouse{probeMouse[0], probeMouse[1]};''')
    replace('const bool cameraInteractionActive = cameraInput.orbiting\n                    || cameraInput.rolling\n                    || cameraInput.panning;', 'const bool cameraInteractionActive = render_fps::dragging(capture);')
    replace('                    && !ImGui::GetIO().WantCaptureMouse\n                    && !cameraInteractionActive', '                    && !cameraInteractionActive')
    replace('            lastFrameTimings = frameTimings;', '            lastFrameTimings = frameTimings;\n            render_fps::end(capture, ui, frameTimings, frameNumber);')
    replace('        updateRuntime.worker.request_stop();\n        if (updateRuntime.worker.joinable())',
            '        render_fps::save(capture);\n        updateRuntime.worker.request_stop();\n        if (updateRuntime.worker.joinable())')
    source.write_text(text, encoding="utf-8")
    shutil.copyfile(Path(__file__).with_name("probe.h"), root / "src/render_fps_probe.h")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("worktree", type=Path)
    instrument(parser.parse_args().worktree.resolve())
