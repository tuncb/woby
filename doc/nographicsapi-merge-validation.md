# NoGraphicsAPI merge validation

Validated on 29 September 2026 in `D:/woby`, after merging
`codex/nographicsapi-migration` at `b25a5a0` into `main` as `7acdf8e`.
The merge retains application version **0.22.1** in both canonical files.
Its only conflict was the CMake project/version block; the new macOS deployment
requirement and main's version were both retained.

## Environment and build

- Windows, Visual Studio 2026, Debug, `vs2026-vcpkg` preset.
- NVIDIA GeForce RTX 3070 Laptop GPU, driver 616.92, 8 GiB VRAM.
- Vulkan SDK 1.4.357.0, standalone Slang 2026.18.3.
- Configure and complete build succeeded with no warnings.
- GPU/headless tests enabled with `WOBY_TEST_HEADLESS=ON`.
- CI configuration tests: 7 passed; CI continues to use Ninja/vcpkg.

## Results

| Check | Result |
| --- | --- |
| Complete configured CTest suite, including slow and GPU checks | 674 passed initially; four headless checks passed after the harness correction below. All 678 checks have passed. |
| Desktop acceptance scripts | All 13 passed in 179.57 seconds. |
| Scene controls and lifecycle | Load, folder import, partial import failures, transforms, visibility, opacity, vertex sizes, save/open/new, dirty policies, retries, errors, undo and redo passed. |
| Rendering and camera | Pixel checks for zoom, pan, roll, look-at, large depth range, close geometry, circular vertices, transparency, and persistence passed. |
| Analysis and annotations | Distance/overlay/quality modes, topology, holes, fins, degenerates, intersections, detector cancellation/invalidation, and large translated annotation surfaces with both up axes passed. |
| Startup exports | Four fresh desktop processes produced eight successful initial/repeated annotated PNGs. |
| Shipped sample scenes | All nine loaded, exported, saved and reopened. Twelve individual report variants produced exactly identical RGB pixels after reload. |
| Import formats and resource reuse | ASCII and binary STL, Unicode paths, and 20 OBJ load/render/remove cycles passed; solid/edge/vertex captures stayed identical across cycles. |
| Many-object stress | 2,500 models imported in 12.32 seconds; render-mode changes, exports, save and reopen passed. |
| Large-model stress | BusGameMap (1,054,542 triangles, 2.26-second import) and Powerplant (12,759,246 triangles, 36.86-second import) passed solid, edge, vertex, orbit and PNG export checks. |
| Historical updater | Actual retained 0.21.3 viewer/helper passed missing-compatibility rejection, corruption rejection, health-check rollback, migration to the merged build, and a subsequent native-only update; user fixture files were preserved. |
| Direct desktop interaction | Comparison heatmap, scene selection and highlight, mouse orbit, detector gear popup placement, Properties pane resizing, maximize, minimize and restore worked. |

The desktop scripts were `ctl_lifecycle_smoke`, `ctl_controls_smoke`,
`ctl_history_smoke`, `ctl_camera_smoke`, `ctl_annotation_render_smoke`,
`ctl_comparisons_smoke`, `ctl_topology_smoke`, `ctl_holes_smoke`,
`ctl_degenerates_smoke`, `ctl_intersections_smoke`, `ctl_fins_smoke`,
`ctl_startup_screenshot_smoke`, and `performance_smoke`.

## Findings and corrections

The initial headless failures came from implicit Vulkan layers injected into the
process, not an SDL viewer window. A controlled diagnostic found a hidden
`temp_d3d_window_*` / `Temp Window` and hidden `IME` / `Default IME` with implicit
layers enabled, and no windows with `VK_LOADER_LAYERS_DISABLE=~implicit~`.
The shared headless session harness now copies the caller's environment and
defaults this setting for its children. Explicit validation layers and caller
overrides remain supported. The strict no-window assertion is unchanged.
All four affected tests then passed: headless, vertex rendering, loading
preparation, and portable importers. No application renderer change was needed.

The headless guide now correctly identifies the Windows backend as NoGraphicsAPI
Vulkan and explains test isolation.

Two existing UI/export constraints were observed:

- A narrow Properties pane wraps detector names into very short lines. Widening
  it restores readability. The diagnostics table layout code is unchanged by
  this merge; this review did not modify it.
- Three paired surface-quality samples exceed the fixed 1800-pixel export's
  report capacity. The app returns the existing explicit capacity error and
  releases the capture queue. Each analysis exported and round-tripped separately.
  Production validation was not relaxed.

The 2,500-object Debug run reported a median frame duration of 158.84 ms, largely
in UI building and graphics submission. This was functional stress testing,
not a Release performance comparison or evidence of a new performance regression.

## Local evidence and scope

Logs are retained under `build/merge-nographics-*.log`; captures and JSON results
are in `build/merge-nographics-qa`. The complete initial CTest output is
`build/merge-nographics-debug-tests.log`; the successful four-test rerun is
`build/merge-nographics-headless-retest.log`. The supplemental harnesses are
`build/merge-nographics-smoke.py`, `build/merge-nographics-samples.py`, and
`build/merge-nographics-large-models.py`. These build artifacts are ignored by Git.
Synthetic model/scene fixtures used unique temporary roots and were cleaned up.
Existing models and source sample scenes were not modified.

This verifies the merged Windows Debug build on the listed NVIDIA device.
Linux, macOS, other GPUs, Release performance, device loss and severe GPU-memory
exhaustion were not tested in this run. No release was published or Git ref pushed.
The pre-existing untracked behavioral comparison report was left untouched.
