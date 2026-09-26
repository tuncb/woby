# Render allocation performance measurements

Measured on 26 September 2026, comparing commit
`13449f38ad0c5a6b80b7fd9972443caaf69b1b14` with the render-allocation changes
described in [the audit](render-allocations.md). Both versions were built in
isolated worktrees under `D:/.worktree`, using Visual Studio 2026 and the
`vs2026-vcpkg` preset with the same installed dependencies.

## Release CPU time

These are milliseconds of main-thread frame work, **excluding the `bgfx_frame`
stage**, which includes renderer synchronization/presentation waits. Each value
is the median of three independent run medians. Lower is better.

| Workload | Before | After | CPU time reduction | Before / after P95 |
| --- | ---: | ---: | ---: | ---: |
| Empty viewport, grid/origin and controls | 0.276 ms | 0.258 ms | 6.8% | 0.375 / 0.384 ms |
| 500 separate files, Objects pane visible | 3.196 ms | 2.600 ms | 18.6% | 4.042 / 3.215 ms |
| Selected file containing 2,048 parts, dimensions enabled | 3.903 ms | 2.798 ms | 28.3% | 4.138 / 2.933 ms |
| Inspector on the last part of a 2,048-part file | 6.754 ms | 3.325 ms | 50.8% | 7.684 / 3.492 ms |
| 100 rectangular annotations on a 3,200-triangle surface | 5.751 ms | 5.861 ms | **−1.9% (slower)** | 6.284 / 6.919 ms |

The inspector improvement is the clearest result: filtering IDs before creating
owning object metadata reduced the UI-build stage from about 4.54 ms to 1.27 ms.
Selection helper submission also benefits substantially from retaining the part,
outline, and dimension-key buffers.

The annotation change reduces allocation churn but does **not** improve Release
CPU time in this workload. Its run medians were 5.741–5.763 ms before and
5.845–5.884 ms after, so the small regression appeared in all three runs. Its
cause was not isolated by this benchmark. The empty-scene improvement is only
about 19 microseconds, and its P95 did not improve.

Frame delivery remained around 120 frames/second in the Release runs, with
VSync enabled. Most savings became additional waiting time in `bgfx_frame`.
These results establish CPU headroom, not a corresponding increase in displayed
FPS or a GPU-rendering speedup.

## Allocation counts, measured separately in Debug

Median main-thread CRT allocation/reallocation events per measured frame:

| Workload | Before calls | After calls | Before requested bytes | After requested bytes |
| --- | ---: | ---: | ---: | ---: |
| Empty viewport | 138 | 121 | 3,670 | 2,224 |
| 500 file rows | 18,090 | 9,122 | 449,862 | 170,432 |
| 2,048 selected parts | 113 | 19 | 4,879,285 | 480 |
| Inspector | 123,376 | 359 | 5,175,912 | 12,222 |
| 100 annotations | 1,049 | 19 | 1,825,437 | 480 |

These counts include Debug STL bookkeeping and therefore must **not** be read as
Release allocation counts. The hook counts allocation/reallocation requests on
the main thread, excluding CRT internal blocks; it does not measure allocations
on bgfx's render thread, in the graphics driver, or in independent heaps. Bytes
are cumulative requested allocation sizes, not live memory, RSS, or bytes copied.
Retained scratch buffers still consume memory after a large frame.

## Method

- Hardware: Ryzen 7 5800H, RTX 3070 Laptop GPU; Direct3D 11 renderer.
- Windowed renderer, 1280 × 720, launched hidden; the ordinary ImGui and scene
  render paths still execute. Both versions retain the normal VSync/MSAA flags.
- Identical synthetic fixtures for both versions, under one unique temporary
  directory. Fixtures were deleted after completion. File/part names exceed
  small-string capacity, representing descriptive model names.
- The selection workload selects the whole file and hides both side panes.
  The inspector workload selects its last part, shows Properties, and hides the
  left pane. The annotation workload hides the panes and opens the same saved
  scene in both versions. All workloads show grid and origin.
- A temporary harness selects the requested object once through UI operations,
  before timing begins. No mouse navigation or gestures are simulated.
- Two-second warm-up, followed by a four-second Release measurement per run.
  Run order was before, after, after, before, before, after. Each version/workload
  contributed approximately 1,420 measured frames across its three Release runs.
- Timing records go into a fixed preallocated array and are written to CSV only
  on exit. HTTP performance queries bracket each interval; three neighboring
  frames at each boundary are discarded. No polling occurs during the interval.
- The identical timing instrumentation is present in both builds. The Debug CRT
  hook is compiled out in Release. Debug allocation runs use five-second windows
  after warm-up, once per version/workload; their timings are not used above.
- No build, test suite, or second viewer ran alongside the measurements. There
  was no CPU affinity, fixed-frequency setting, or thermal control. Synthetic,
  static workloads do not establish the same gains for every real scene.

All four instrumented builds (before/after, Debug/Release) completed without
compiler warnings. No production implementation changes were made during this
measurement pass; the prior 622-test validation still applies to that code.

## Data and local reproduction artifacts

[Aggregate CSV](render-allocation-results.csv) includes run ranges, P95 values,
sample counts, stage medians, and Debug allocation/byte counts. Raw per-frame CSVs,
`runs.jsonl`, and `summary.json` are retained locally in
`build/render-benchmark-results/`.

The local driver is `build/render_allocations_benchmark.py`; instrumentation is
`build/render-benchmark-profile.h`, also copied into each benchmark worktree as
`src/render_benchmark_profile.h`. The only harness edits in those copies include
that header, construct `Capture` before the main loop, call `prepare`/`begin`
before `frameStart`, and call `end` after `totalMilliseconds` is calculated. The
driver creates fixtures through the normal API, alternates launches, requests
clean shutdown, and removes its temporary fixtures even on failure.

To reproduce using those retained worktrees, run the driver from the repository
root after both configurations are built. It appends to `runs.jsonl`; use a fresh
output directory when comparing a new change. The Release allocation columns in
raw CSVs are zero placeholders because the hook is disabled, not measurements of
zero whole-frame allocations.
