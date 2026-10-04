# Native keycap loading — issue #102

Investigation on 4 October 2026, based on `b79ad95`, using the Visual Studio
2026/vcpkg preset and Release CPU benchmarks on the issue's Windows test machine.
The [issue](https://github.com/tuncb/woby/issues/102) records **131.53 seconds**
for the 500-copy model in Woby 0.26.0. That is whole `model.add` latency, not
a measurement of spline evaluation alone.

## Cause and fix

Each keycap has seven trimmed surfaces. Six first build a 129 × 65 parameter
grid (8,385 vertices each), and one builds a 33 × 33 grid. The triangulator inserts
all grid vertices and constrains all grid edges before filtering out triangles
outside the trim loops. Across one keycap, 51,399 initial grid vertices become
only 2,234 retained vertices, including newly inserted boundary intersections.
Individual 8,385-vertex grids retain as few as 112 vertices.

The fix computes the union of the validated outer trim-loop bounds and supplies
only the intersecting grid cells, with their enclosing grid lines, to CDT.
Boundary sampling, tolerances, hole validation, grid spacing, knot constraints,
surface evaluation, normals and cancellation are unchanged. Keeping a neighbor
at exact grid-line boundaries avoids dropping boundary cells. The original
parameter arrays remain intact; only the temporary CDT input is reduced.

The CDT capacity guard now checks this reduced input before allocation. The
whole-mesh index checks still apply. The old two-million-element freeform limit
mentioned in the issue had already been removed from the current base commit;
the 1,000-copy model is now a successful-load case, not an expected rejection.

## Measurements

The new `woby_benchmarks load model.obj [repetitions]` workload records elapsed
time between native importer progress stages, excluding viewer/GPU preparation.
These are CPU-workload **wall times**, not sampled CPU stacks. Builds and other
machine activity overlapped these initial runs, so their absolute times should
not be compared directly with the historical 131.53-second viewer result.

| Native import stage, 500 copies | Before (s) | After (s) |
| --- | ---: | ---: |
| Reading/parsing | 2.083 | 2.644 |
| Triangulation | 222.871 | 10.349 |
| Building geometry | 2.190 | 1.188 |
| Bounds | 0.031 | 0.021 |
| Total | **227.175** | **14.202** |

Both runs produced **1,117,000 vertices, 1,497,500 triangles and 3,500 groups**.
The original 1,000-copy corpus model loaded in **36.520 seconds**, producing
2,234,000 vertices, 2,995,000 triangles and 7,000 groups.

A separately instrumented single-keycap baseline attributed trim time to:

| Trim substage | Elapsed milliseconds |
| --- | ---: |
| Boundary curve sampling and surface evaluation | 8.160 |
| Loop validation | 1.062 |
| CDT input preparation | 48.364 |
| CDT vertex insertion | 381.063 |
| CDT edge insertion | 86.902 |
| Triangle filtering/finalization | 201.344 |

Thus the dominant cost is triangulating and filtering unused surface area.
The separate position/normal evaluations during final mesh construction account
for a small part of total import time and were left unchanged.

Raw local evidence is under `D:/woby-build/issue-102`: `before-500.txt`,
`after-500.txt`, `after-1000.txt`, and `profile-single.txt`. The instrumented
translation unit is `profile_trim.cpp`; instrumentation is not in production.
The 500-copy source is the archived derivative at
`D:/woby/build/large-file-stress-20261003/derived-models/parametric/SA_Row3_x00500.obj`.
The 1,000-copy source is `D:/temp/obj_tests/parametric/SA_Row3_x01000.obj`.

Fresh headless Vulkan viewer runs also completed. The archived 0.26.0 viewer's
500-copy `model.add` took **213.898 seconds** (193.484 sampled process CPU seconds);
the patched viewer took **17.572 seconds** (17.594 sampled process CPU seconds).
Both reported identical vertex/triangle/group counts and bounds. Captures were
inspected; this is not an FPS comparison or a claim of pixel-identical output.
The patched viewer loaded the original 1,000-copy model in **33.685 seconds**.
These runs use different application baselines as well as the trim fix, so the
CPU-only same-base comparison above isolates the tessellation change better.

The viewer evidence is in `viewer-before-500`, `viewer-after-500` and
`viewer-after-1000` below the same evidence root. Committed
[measurement metadata](benchmarks/freeform-loading-20261004.json) records binary
hashes, command latency, CPU time, geometry counts and model metadata.

## Load deadline

The 600-second setting belonged to `tests/stress/run.py`. It bounded automated
campaign waits and cleanup; the interactive importer has no such tessellation
time limit. It neither caused the slow work nor made it faster.

The harness now defaults to `--load-timeout 0` (no overall load deadline).
After the initial 60-second RPC wait, a running command is followed using
`command.get` until its existing result is available. It is never submitted
again. Process exit, transport errors, cancellation and explicitly enabled
memory cutoffs remain terminal. Non-load operation deadlines remain unchanged.
Use `--load-timeout 600` for historical reproduction.

## Validation

Regression coverage checks retained grid crossings and constraints, non-unit
parameter domains, exact grid boundaries, narrow retained regions and early
capacity decisions for a mostly discarded 50,000 × 50,000 grid. Existing tests
cover rational circular holes, multiple regions, invalid-loop diagnostics,
determinism and cancellation. Harness tests cover no-deadline command polling,
success/failure propagation, process exit, transport errors, and explicit
deadline behavior.

Debug and Release builds completed without compiler warnings. All **857 CTest
checks** passed, including the four slow tests, and all **24 harness tests** passed.
The original viewer's 213.898-second load exercised the new no-deadline polling
path through successful completion. The 1,000-copy patched viewer run retained
the historical `--load-timeout 600`; all viewer runs explicitly retained the
38 GiB private-memory and 6 GiB available-memory cutoffs.

Reproduce the stage benchmark after configuring with
`cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON`:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_benchmarks
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe load D:/woby/build/large-file-stress-20261003/derived-models/parametric/SA_Row3_x00500.obj 3
```
