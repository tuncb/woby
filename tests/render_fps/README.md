# Full-viewer rendering experiment

This is research instrumentation, not a production feature. `instrument.py`
patches `main.cpp` only in a disposable worktree; renderer, mesh, hover-picking,
shader and UI implementations remain the production versions. The injected
adapter controls camera and display settings through UI operations. Native
mouse coordinates are replaced with a deterministic pointer position, and the
drag gate and ImGui mouse-capture gate are controlled so the production hover
path can be exercised in a hidden window. SDL event delivery, physical mouse
latency and the actual keyboard adapter are not benchmarked.

The normal multithreaded bgfx renderer, ImGui and presentation paths execute.
The probe requests NVIDIA hardware and Direct3D 11. Default experimental settings
are a 1280×720 window, four-sample MSAA, VSync off, panes/helpers hidden, opaque
solid mesh and the isometric camera fitted to the complete model. Explicit
scenarios vary these settings. The VSync scenario restores the production reset
flags. This is windowed frame submission/delivery, not a measurement of physical
monitor scanout, input latency or an offscreen scene-only benchmark.

Each scenario warms for at least 1.5 seconds and 20 frames, then measures at least
3 seconds and 30 frames. Camera motion follows a repeating two-second path:
orbit ±30°, pan ±0.15 fitted camera distances, zoom by `exp(±0.55)`. Near and far
views use 0.5× and 4× the fitted distance. Keyboard movement is represented by
camera-local translation without a drag flag. Pointer sweep moves ±15% of the
viewport width. Static pointer tests warm the hover cache before measurement.

Per-frame records remain in memory until shutdown. There is no HTTP polling,
per-frame file output or screenshot capture during the reported sweep. FPS is
frame count divided by elapsed frame intervals. CPU work excludes the
`bgfx_frame` stage, which mixes GPU/render-thread/presentation waiting. GPU times
come from bgfx timestamps; repeated GPU frame IDs are deduplicated, and only IDs
inside the measurement's source-frame range with an eight-frame boundary margin
are included. GPU and CPU times overlap and must not be added. GPU clocks can
downclock during CPU-bound intervals, so GPU timings from those intervals do not
measure the saturated renderer's throughput.

Two independent process runs per model vary scenario order, with reversed model
order in the second round. Optional buffers are retained after first enable,
matching production behavior; memory residency can consequently differ with
scenario order. Small differences must be interpreted with the recorded run
ranges. No CPU affinity, fixed GPU frequency or thermal lock is applied.

From a clean worktree at the recorded commit:

```powershell
git worktree add --detach D:/.worktree/woby-render-fps HEAD
uv run tests/render_fps/instrument.py D:/.worktree/woby-render-fps
cd D:/.worktree/woby-render-fps
cmake --preset vs2026-vcpkg -DVCPKG_INSTALLED_DIR=D:/woby/build/vs2026-vcpkg/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF
cmake --build --preset vs2026-vcpkg --target woby
cmake --build --preset vs2026-vcpkg --config Release --target woby
cd D:/woby
uv run tests/render_fps/test_analysis.py
uv run tests/render_fps/run.py --binary D:/.worktree/woby-render-fps/build/vs2026-vcpkg/bin/Release/woby.exe --models D:/temp/obj_tests --output D:/woby/build/render-fps-results-new --rounds 2
uv run tests/render_fps/run.py --binary D:/.worktree/woby-render-fps/build/vs2026-vcpkg/bin/Release/woby.exe --models D:/temp/obj_tests --output D:/woby/build/render-fps-extra-new --rounds 1 --suite extra
uv run tests/render_fps/summarize.py build/render-fps-results-new/runs.jsonl build/render-fps-extra-new/runs.jsonl build/render-fps-results-new/summary.json
uv run --with matplotlib tests/render_fps/plot.py build/render-fps-results-new/summary.json build/render-fps-results-new/summary.png
```

Use a fresh output directory and run serially with no other viewer, build or test
suite active. The shared dependency directory must already exist. The driver
launches the window hidden and terminates on its own after all scenarios; a failed
or timed-out run is rejected. Model files are never changed. `--captures` is for
separate visual QA runs; bgfx writes TGA files during warmup. Unit-test fixtures
use one unique temporary directory and are removed even on failure.

The core suite has 26 scenarios. Fifteen extra scenarios match zoom/pan paths while
changing hover eligibility, restore production presentation flags plus the pane,
test a viewport pointer during solid zoom, disable MSAA for points/edges, and
test transparent markers and zero-opacity solid surfaces. They also compare a
near plane of `max(original near plane, 0.01 × fitted distance)` against unchanged
solid/point/edge reference cases in the same process. Focused controls use one
process run per model, so treat small differences as exploratory. Keep Debug results in
a separate aggregate. `plot.py` exports the report chart from a combined Release
aggregate; run it with `uv run --with matplotlib`.
