# Large-file performance measurements

These opt-in scripts measure the unmodified viewer and existing native benchmark. They are not timing assertions in the unit-test suite. The [path catalog](../../doc/large-file-stress-paths.md) explains the test surface; the [October 2026 report](../../doc/large-file-stress-results.md) records the baseline and evidence.

Run from the repository root in PowerShell. Build and test before measuring. Run one campaign at a time, with no concurrent build or test suite.

```powershell
cmake --build --preset vs2026-vcpkg --target woby woby_tests woby_benchmarks
ctest --preset vs2026-vcpkg --output-on-failure
ctest --test-dir build/vs2026-vcpkg -C Debug -L slow --output-on-failure
cmake --build --preset vs2026-vcpkg --config Release --target woby woby_benchmarks
uv run --with psutil tests/stress/test_runner.py

uv run --with psutil tests/stress/run.py `
  build/vs2026-vcpkg/bin/Release/woby.exe D:/temp/obj_tests `
  build/stress-load-new --suite load

uv run --with psutil tests/stress/run.py `
  build/vs2026-vcpkg/bin/Release/woby.exe D:/temp/obj_tests `
  build/stress-render-new --suite render --select BearTrap --rounds 2 `
  --seconds 8 --warmup 2
```

Output directories must be new. `--select` matches filename substrings and can repeat. Files are sorted by size and reversed on alternating rounds. OBJ files below `sources` are excluded. With no selection, every other OBJ under the corpus root is attempted. Each case gets a unique temporary working directory on the output drive, cleaned even on failure. Persistence fixtures put scene and model files on that same drive; immutable input hard links fall back to copies across drives. Original inputs are not edited.

| Suite | Workload |
| --- | --- |
| `load` | Empty startup, import, annotation preparation, default rendering and capture |
| `startup` | `--file` launch-to-ready measurement |
| `render` | Pane, camera motion, solid/edge/vertex/combined modes, point sizes, transparency, UV grid and hidden control |
| `controls` | Near/far camera, roll/move, helpers, transforms/color, line width/depth, up axis, saved views |
| `mesh` | Nine default detectors, four quality metrics, topology modes, thresholds, findings, full export and invalidation |
| `analysis-display` | Pane/overlay controls, later findings page, focus and export cancellation |
| `intersections` | Bounded self-intersection run, findings, cancel and retry |
| `uv` | Inspection/quality, layout, six metrics, normalization, thresholds, overlap, pagination and probes |
| `uv-display` | Whole and closer quality layouts with explicit Z-up/front camera |
| `uv-probe` | Valid later-page UV triangle and linked probe |
| `comparison` | Identical A/B, both distance directions, five modes, tolerance, cache and swap |
| `comparison-offset` | Translated independent copy, nonzero distances, side enable/disable and swap |
| `distance-only` | Comparison with automatic detectors disabled; full results still include surface quality |
| `concurrent` | Three queued analyses, camera motion, invalidation and deletion |
| `lifecycle` | Inventory/tree, visibility, undo/redo, transforms/views, save, populated/empty reopen and clear |
| `analysis-lifecycle` | Save/reopen all three analysis families and await restored results |
| `analysis-members` | Group/file scope, add/remove/clear, enable/isolate and group-transform invalidation |
| `annotation` | Project line/rectangle, edit, render, source transform, delete and undo/redo |
| `retention` | Five import/display/remove/history-clear cycles, empty-scene memory |
| `batch` | Flat/tree folder copies, mixed failure, repeated-path import, pane on/off; `--batch-count` sets count |

`--headless` is limited to load/lifecycle checks; it is not an FPS proxy. Defaults are a four-second frame window, 1.5-second warm-up, no load deadline (`--load-timeout 0`) and a 240-second operation deadline. Long loads are followed through `command.get` after the initial RPC wait expires; they are never resubmitted. Process exit, transport failure and explicit memory cutoffs still stop the case. Use `--load-timeout 600` to reproduce the historical ten-minute harness deadline; this is not a viewer tessellation limit. Memory is sampled every 200 ms without a default memory cutoff. The viewer reports allocation failures and stops the affected analysis. Optional `--max-private-gib` and `--min-available-gib` cutoffs are disabled at `0`; historical campaigns can be reproduced with `--max-private-gib 38 --min-available-gib 6`. Explicit cutoffs can overshoot between samples. Manifests record actual settings, executable/script hashes, runtime versions and model paths/sizes/timestamps. Deadline/explicit-cutoff cleanup terminates only the case's own viewer.

`status=completed` means the scripted workflow finished. Inspect RPC responses and detector statuses before declaring an analysis complete. Rejection, truncation, process failure, and harness termination are distinct outcomes. The authenticated local RPC token is never written into evidence.

Harness version 4 launches a visible, fixed-size desktop window by default using
`--drawable-size`, with `--width 1280 --height 720` as the default pixel dimensions.
`--hidden-window` is an explicit control and excludes desktop FPS. Headless mode
still skips desktop frame measurements. Scene and Properties visibility are
independent (`pane.set` parameters `visible` and `propertiesVisible`).

Each FPS window decodes a separate PNG capture and rejects missing, corrupt or
blank output before aggregation. It warms up, then records bounded complete frame
events with `performance.begin/end`. The 16,384-frame cap reports dropped events;
overflow, missing/nonmonotonic events, changed drawable/viewport/DPI/panes/pacing,
scene edits, unexpected camera motion, or failed presentation exclude the entire
window. Both raw observations and exclusion reasons remain in `frames.jsonl`.
FPS uses completed frame intervals on the viewer's monotonic clock. P95/P99 are
completion-interval statistics from this complete sequence, not GPU latency or
physical scanout. Sparse polling timings retain the `sampled_` prefix.

PNG export has separate dimensions and happens outside measurement. Nonblank
captures and nonempty geometry counts are necessary checks, not proof of a correct
diagnostic overlay: inspect captures and detector/display status before claiming
an optimization. Physical visibility/occlusion is unknown; window flags and actual
swapchain submissions are recorded. GPU telemetry remains device-wide. RPC polling,
resource monitoring and optional performance logging still add overhead.

Before measuring optimizations, run this small matrix serially at two resolutions
with three independent processes per setting (four Scene/Properties combinations).
Use fresh output paths and keep builds/tests closed during measurement:

```powershell
uv run tests/stress/run.py build/vs2026-vcpkg/bin/Debug/woby.exe assets/samples/surface-mesh-quality build/benchmark-validation-new/720p --suite validation --select equilateral --rounds 3 --width 1280 --height 720 --seconds 3 --warmup 1.5 --max-private-gib 38 --min-available-gib 6
uv run tests/stress/run.py build/vs2026-vcpkg/bin/Debug/woby.exe assets/samples/surface-mesh-quality build/benchmark-validation-new/600p --suite validation --select equilateral --rounds 3 --width 800 --height 600 --seconds 3 --warmup 1.5 --max-private-gib 38 --min-available-gib 6
uv run --with psutil tests/stress/summarize.py build/benchmark-validation-new build/benchmark-validation-new/summary.json
```

The runner and its unit tests declare psutil/Pillow dependencies for `uv run`.
Repeat aggregation requires successful cases, verified captures, identical binary
hashes and matching scene/camera/environment keys; it reports run count, FPS range
and sample standard deviation. Three comparable repeats establish an initial
variance estimate. A single run is never labeled as establishing variance. Old
records lacking this validation remain raw evidence and do not enter this aggregate.
Keep focus consistent and avoid input during each window. Windows may leave a
new visible window unfocused; those repeats stay in a separate comparison group.
If no group has three matching repeats, collect additional runs in fresh output
directories rather than averaging mismatched focus/presentation states.
Keep Debug and Release campaigns separate. Record and compare actual viewport
dimensions: changing panes intentionally changes the workload.

Cache policy: each case starts a fresh viewer, each window warms for the configured
period, and OS file/driver caches are uncontrolled. This is not a cold-cache claim.
Manifests retain binary/script hashes, Python/psutil/OS versions and settings; raw
runtime samples include application/SDL versions, build configuration and renderer,
while NVIDIA telemetry includes driver version when available. No CPU affinity,
GPU clock lock, thermal control or compositor timing control is applied.

CPU-only stages:

```powershell
uv run --with psutil tests/stress/cpu.py `
  build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe D:/temp/obj_tests `
  build/stress-cpu-new --select BusGameMap --repetitions 3
```

The native driver's stage is a bitmask. It can group topology, duplicate and degenerate work into one call; it is not an individual timing for every detector. Viewer snapshots, GPU uploads and UI are outside stage timings. The wrapper records memory without default memory cutoffs and applies a configurable process deadline. It accepts the same optional `--max-private-gib` and `--min-available-gib` cutoffs as the viewer runner.

Summarize a parent directory of campaigns:

```powershell
uv run --with psutil tests/stress/summarize.py `
  build/large-file-stress-20261003 doc/benchmarks/large-file-stress-20261003.json
```

The JSON includes case outcomes, operation timings, analysis summaries, phase memory, CPU load events, CPU benchmark output and manifests. A sibling CSV contains frame scenarios. Retain raw `operations.jsonl`, `frames.jsonl`, `resources.json`, `viewer.log`, GPU CSVs and captures. Full diagnostic exports remain separate large artifacts.

Render the baseline figures:

```powershell
uv run --with matplotlib tests/stress/plot.py `
  doc/benchmarks/large-file-stress-20261003.json doc/benchmarks/large-file-stress-20261003
```

The retained `native_driver.py` in the evidence archive wraps the same recorder and accepts numbered JSON requests while an operator uses the visible app. `native-ui/.../native-actions.jsonl` and `native-observations.json` record the sequence and its limitations. Native UI timings are not inferred from automation tool durations.

That one-off driver expects to live at `build/<campaign>/native_driver.py` under the repository; copy it there before reproducing. Launch it like `run.py` with `--suite native --visible-window`, wait for `native-ready.json`, then write sequential `native-request-000.json` files in the case directory. Supported actions are `measure`, `rpc` and `finish`; use atomic file replacement, wait for each response, and keep UI changes outside measurement windows.

Create the explicitly labeled finite-only SCARED derivative:

```powershell
uv run tests/stress/prepare_derivatives.py `
  D:/temp/obj_tests/pointclouds/scared_c_keyframe1_vertices.obj `
  build/stress-derived-new/pointclouds/scared_c_finite_vertices.obj
```

The helper rejects indexed geometry, refuses existing output and records both hashes and discarded-row counts. The 500-copy keycap derivative uses the corpus's `tools/prepare_benchmarks.py` `replicate` function with its `ROOT` set to the new derivative directory, keeping metadata there. Its provenance and source license accompany the measurements.
