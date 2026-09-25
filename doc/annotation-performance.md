# Rectangular annotation performance

## Gesture-local projection (25 September 2026)

On `uploads_files_2720101_BusGameMap.obj` (1,054,542 triangles, 65 groups),
the 1920-by-1080 top-view benchmark found the same 574-segment rectangle at
NDC (-0.23, -0.38) to (-0.17, -0.32). Model import and the search for that
placement are outside the timers. The figures below are medians of three runs
on this placement; they do not represent every camera view or rectangle.

| Stage | Previous Debug | Current Debug | Previous Release | Current Release |
| --- | ---: | ---: | ---: | ---: |
| Mouse-down to active gesture | 3,901.4 ms | 91.5 ms | 352.5 ms | 12.1 ms |
| Release and attach | 341.7 ms | 165.7 ms | 32.8 ms | 32.2 ms |

The first drag update expanded the projection region in 116.7 ms in Debug and
13.1 ms in Release. Nine repeated updates at the same pointer position then
averaged 2.63 ms and 0.18 ms respectively. These are different stages of one
drag, so the previous average over ten updates would hide the initial pause.
Exact outline resolution measured 13.8 ms and 0.93 ms respectively. The
committed geometry matched the full projection, segment for segment. These are
CPU operation timings, not viewer frame times.

Large models now build source-space blocks and source fingerprints when loaded.
Mouse-down projects only blocks around the pointer. The projection grows as the
rectangle grows; bounds checks are conservative, so relevant triangles are
still considered. The sampled guide remains temporary, and release resolves
the outline exactly and rechecks the current source geometry. Caching moves
some work into model load, which is outside this benchmark. Debug source
fingerprint validation is faster because its hash loop avoids repeated
function calls while retaining the saved fingerprint format. Vertex-cache
generation stamps avoid clearing a whole-mesh array for each part, and line
submission reuses each source transform.

## Camera navigation investigation (23 September 2026)

The supplied `slow_rectangle.woby` contains one visible rectangle with 761
segments on the 1,054,542-triangle map. In the Debug viewer, a 360-degree
scripted yaw sweep with varying pitch measured a median 119.96 FPS both with
the rectangle visible and hidden. GPU frame time was approximately 0.6 ms.
A smaller alternating rotation measured median helper submission times of
3.708 ms visible and 0.378 ms hidden. These measurements include other scene
helpers and are not isolated annotation draw timings. The frame rate is capped
by presentation; hiding the rectangle increases waiting time rather than FPS.

A subsequent manual mouse-rotation check with the rectangle explicitly visible
also stayed fast. The reported unselected-annotation drop to 20 FPS has not been
reproduced in this checkout's Debug build; these results do not establish its
cause or demonstrate that it is fixed.

A separate selected-annotation problem was reproduced: the handle overlay
treated one unchanged camera frame as settled navigation, even when a drag was
still active. It could then perform four full-mesh visibility picks between
mouse events. Handle visibility now waits until scene pointer interaction is
available again. The regression test covers unchanged frames during navigation
and restoration of the handles after navigation ends. The visibility work still
runs after navigation ends; this change does not accelerate those mesh picks.

On this scene, the updated benchmark's median navigation overlay time was
0.082 ms per frame across three runs. The settled visibility update still took
about 3.2 seconds in Debug. Before the guard, that work ran during navigation:
the first baseline run averaged 1,711 ms per frame over changed/unchanged pairs.
This is deferred work, not an improvement to the picking algorithm. The Debug
build completed without compiler warnings and all 567 CTest entries passed,
including the graphics and slow tests.

An opt-in benchmark exercises the real saved scene, alternating a camera change
and an unchanged frame while pointer interaction is unavailable, then timing
the settled visibility update separately. It measures CPU overlay construction,
not GPU rendering or complete viewer FPS. Model loading is outside the timers:

```powershell
$env:WOBY_ANNOTATION_SCENE = 'C:\path\to\slow_rectangle.woby'
& build/vs2026-vcpkg/bin/Debug/woby_tests.exe `
  '--test-case=annotation navigation overlay external scene benchmark' --no-skip
```

## Annotation creation measurements

Measured on 22 September 2026 with the Visual Studio 2026 `vs2026-vcpkg`
preset. Timings use `steady_clock`, three runs, and the median. Mesh import and
fixture construction are outside the annotation timers. The synthetic workload
averages ten rectangle updates per run. These are CPU timings, not frame times.

## User-supplied map

`uploads_files_2720101_BusGameMap.obj` contains 1,054,542 triangles in 65 groups.
The benchmark uses a 1920-by-1080 top view with Y up, on the largest group,
`Ocean_Middle_Medium_Detail.001_Plane.031`. The valid rectangle runs from NDC
(-0.23, -0.38) to (-0.17, -0.32), approximately 58 by 32 pixels. It crosses
574 exact surface segments. Results characterize this placement, not every
possible view or rectangle on the map.

| Stage | Updated Debug median | Updated Release median |
| --- | ---: | ---: |
| Mouse-down, unselected model | 3,901.4 ms | 352.487 ms |
| Sampled drag update | 2.277 ms | 0.166 ms |
| Exact outline resolution alone | 13.999 ms | 0.921 ms |
| Release, including exact resolution and attachment validation | 341.706 ms | 32.773 ms |

The exact and sampled timings above are two paths in the updated executable,
not a comparison against an old executable on this map. Mouse-down remains
noticeable in Debug; attachment fingerprint validation also contributes to
release time.

## Synthetic before/after comparison

The preserved baseline executable uses production code from `2f9decb`, plus the
same synthetic benchmark fixture. Baseline, updated Debug, and updated Release
runs were sequential, with builds, viewers, and other tests stopped. No cold
cache or thermal control was applied. These curved grids and overlapping layers
exercise triangle count and overdraw, not every distribution of CAD/map faces.

| Geometry | Baseline Debug mouse-down | Updated Debug mouse-down | Baseline exact update | Updated Debug drag guide |
| --- | ---: | ---: | ---: | ---: |
| 20,000 triangles | 209.986 ms | 73.066 ms | 9.514 ms | 10.531 ms (exact) |
| 980,000 triangles | 13,157.9 ms | 3,974.8 ms | 83.438 ms | 2.199 ms |
| 1,000,000 triangles, eight layers | 14,882.2 ms | 3,727.8 ms | 132.981 ms | 6.786 ms |

For the two dense fixtures, this is approximately 3.3–4.0 times faster on
mouse-down and 20–38 times faster per drag update. The original exact-update
timer measures the projection call; the new drag timer also includes pointer
handling. Full exact projection itself is slightly slower with the compact
representation: 87.263 and 143.932 ms on these fixtures. The responsiveness gain
comes from avoiding repeated full resolution while dragging, not from making
that calculation faster.

Updated Release mouse-down times were 333.387 and 366.266 ms for the two dense
fixtures; sampled drag times were 0.296 and 0.631 ms. There is no preserved
Release baseline, so these are absolute timings, not Release speedup claims.

The 980,000-face cache's live data payload falls from a calculated 275.5 MB
with the original records to 124.6 MB with compact records, about 55% less.
This counts vector elements, excluding capacity slack, allocation overhead,
temporary sorting buffers, the source mesh, and application/GPU memory.

## Validation

The complete Debug CTest run passed all 556 entries, including its four slow
tests and two graphics tests. Both configurations compiled without warnings.
The annotation unit tests also passed in Release. The separate annotation render
smoke test passed on large translated terrain with both up axes, both shapes,
and three near planes.

New regression cases cover cache ownership, transparent target selection,
deterministic point traversal, bounded perspective guides, exact geometry after
dense-mesh creation and editing, cancellation, scene changes, and narrow
occluders missed by the guide but rejected on release.

## Changes

- Store transformed vertices once, with compact face references. Only triangles
  clipped by the viewport need explicit interpolated barycentric records.
- Build the projection hierarchy with a stable radix sort of Morton keys. Ties
  retain source order, including coincident faces.
- Reuse the discovery projection when an unselected model becomes the target.
  Remove transparent neighbors without transforming the scene again.
- Traverse the hierarchy directly for point samples, without allocating and
  sorting a separate candidate list each time.
- Above 50,000 projected faces, use a temporary guide with 32 samples per side
  (at most 128 segments per rectangle). On release, resolve the complete outline
  and validate its attachment before changing scene state.

The guide can miss a narrow obstruction or bridge across detail. It stays visible
over the model because sampled chords can fall below a curved surface. The
viewport message says to release to attach the outline. Full validation on
release still rejects occlusion and disconnected surface layers. Small models
continue to use exact previews. Saved annotations, source attachment, undo/redo,
and the `.woby` format retain their existing exact geometry.

## Reproduction

Build and run the opt-in synthetic workload in PowerShell:

```powershell
cmake --build --preset vs2026-vcpkg
& build/vs2026-vcpkg/bin/Debug/woby_tests.exe `
  '--test-case=surface annotation large mesh benchmark' --no-skip
```

For a local OBJ, use the external workload. It frames the model from above with
Y up, chooses the largest group, searches for a valid rectangle, then measures
mouse-down, guide updates, exact resolution, and the complete release operation.
It verifies that the committed geometry equals the exact result. A model without
a valid rectangle in this view will fail the workload's placement assertion.

```powershell
$env:WOBY_ANNOTATION_MODEL = 'C:\path\to\model.obj'
& build/vs2026-vcpkg/bin/Debug/woby_tests.exe `
  '--test-case=surface annotation external model benchmark' --no-skip
```

For optimized measurements, build with `--config Release` and use the executable
in `bin/Release`. Run benchmarks sequentially with builds and other tests stopped.
The workloads have no timing assertions and are skipped in ordinary test runs.

## Remaining work

Projection preparation still visits the whole visible mesh on mouse-down.
Moving this work to a cancellable background job, or retaining a mesh-space
hierarchy across gestures and camera changes, would address the remaining start
latency. Permanently simplifying stored outlines could also reduce rendering
cost with many annotations, but needs a surface-error tolerance and rendering
that prevents simplified chords from disappearing inside curved models.
