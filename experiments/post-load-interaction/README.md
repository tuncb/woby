# Post-load interaction probe

This probe measures UI selection queries, inspector queries, viewport clicks, and
logical removal after loading and preparation have finished. It calls the current
production functions. It does not change input files.
The CPU probe runs without a renderer. Its click results do not include frame
presentation, mouse event dispatch, or panel layout changes.

Build and check the probe in Release mode:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby woby_post_load_interaction_probe
build/vs2026-vcpkg/bin/Release/woby_post_load_interaction_probe.exe --self-test
```

Measure the specified model. The source path must be absolute. Each CPU operation
has three samples in one process, except loading, preparation, and removal.
Center clicks must return the group ID. Corner clicks must return zero.
The output separates changing selection from the geometry queries that follow it.

```powershell
build/vs2026-vcpkg/bin/Release/woby_post_load_interaction_probe.exe D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj build/beartrap-interaction-cpu.json
```

Add `--verify` to measure an 81-point combined-mode grid and check one hit and
one nearby miss against exhaustive traversal. This mode also checks persistent
triangle IDs and barycentric coordinates. Add `--retire` instead to measure
logical removal separately from releasing an owned geometry snapshot on a worker.
The retirement check verifies that the scene is empty while the snapshot is alive
and that the geometry is released after the worker finishes. It changes the probe
only. The JSON output records triangle/node visit counts for each click.

Measure rendering and removal in the desktop viewer:

```powershell
uv run experiments/post-load-interaction/render.py build/vs2026-vcpkg/bin/Release/woby.exe D:/temp/obj_tests build/beartrap-interaction-render --suite render --select 20190810_BearTrap_Ground_Model --rounds 1 --seconds 2 --warmup 1 --operation-timeout 240
```

The render script uses `tests/stress/run.py` for process ownership, readiness
checks, complete frame captures, screenshots, and resource monitoring. It waits
for post-load and annotation preparation. It measures solid, triangle-edge,
vertex, and combined modes. It then removes the model in combined mode, reloads
it, and removes it in solid mode. It checks the resulting empty scenes.

Check selected-model UI frames through temporary saved views:

```powershell
uv run experiments/post-load-interaction/selection.py build/vs2026-vcpkg/bin/Release/woby.exe D:/temp/obj_tests build/beartrap-selection-frames --suite lifecycle --select 20190810_BearTrap_Ground_Model --rounds 1 --operation-timeout 240 --width 1920 --height 1080
```

This workflow uses the harness's unique temporary fixture directory and its model
hard-link/copy helper. It saves and edits its own scene to set the selected file in
one view. It reopens that scene, captures three selected/cleared frame windows,
and removes the selected model. The source model is not edited. The saved-view
fixture tests normal selection-dependent UI work without injecting mouse events.

Set `WOBY_INTERACTION_DENSE_RENDER=1` to disable adaptive points and enable X-ray
edges. Use `--width 3840 --height 2160` for the larger viewport control. Remove
the environment variable after the run. Outputs must use a new directory for
each run. Other options are those of the stress harness.

Do not run builds or tests during a measurement. Three windows in one process
describe that run; they do not establish variation between independent runs.
