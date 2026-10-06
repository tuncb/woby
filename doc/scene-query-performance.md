# Scene query caching

Issue [#114](https://github.com/tuncb/woby/issues/114) extends the Properties
revision model to repeated scene queries. `SceneQueryRuntime` owns cached tree
summaries, membership sets, composed part metadata, selection framing bounds,
exact dimensions, comparison inputs, and annotation world/projection geometry.
The viewport, camera toolbar, and Properties share the runtime.

## Dependencies and ownership

Every scene cache checks the `UiState` owner and document generation. Operations
advance domain revisions; undo/redo advances all domains and New/Open changes
the document generation. Repeated reads compare keys before allocating or
walking the hierarchy.
Live resource and result keys can still iterate files or comparison runtimes;
cache hits do not traverse hierarchy nodes or scan mesh vertices/indices.

| Query | Relevant changes | Independent changes |
| --- | --- | --- |
| Tree counts | Geometry/hierarchy, visibility | Labels, colors, camera, selection |
| Membership sets | Active comparison, analysis membership | Colors, camera, ordinary selection |
| Source part transforms/ranges | Geometry/hierarchy, visibility/opacity/render modes | Labels, colors, camera, selection |
| Part selection/style | Selected IDs; picking revision for sizes and line styles | Transform composition is retained |
| Selection framing | Source contributors, selection, annotation geometry/visibility, comparison inputs/display | Labels, colors, camera, result publication |
| Exact dimensions | Source contributors, selection, comparison inputs/display/focus, CPU results and GPU readiness | Labels, colors, camera, annotation strokes |
| Comparison signatures | Geometry, analysis sources and UV input settings | Labels, visibility, colors, camera, detector publication |
| Annotation world lines | Source geometry/visibility, per-annotation outline revision, visibility, target validity | Labels, RGB, width, camera |
| Annotation projection | World lines, view/projection matrices, viewport, pixel scale, width | RGB, labels |
| Hover scene identity | Geometry, visibility, picking styles, mesh content and GPU resource identity | Labels, colors, camera/mouse (separate query inputs) |

Framing bounds include visible local boxes even when primitive display is off;
exact dimensions measure rendered selected geometry. Comparison display bounds
use a source/layout signature separate from UV metrics and detector results.
Readiness still checks settings, result revisions and uploaded stage revisions.

Part records own transforms, IDs, ranges and ancestor IDs. They retain no mesh
pointers, vertex/index copies, point-cloud pointers or diagnostic spans. Consumers
resolve short-lived views against the current file/mesh identity, including after
the scene's file vector relocates. Annotation caches own their derived lines and
projected triangles. GPU resources keep an independent identity.

Annotation workers check explicit mesh content revisions, document/owner identity,
relevant scene revisions, camera/depth bounds and selection before publication.
The preparation worker still joins before borrowed geometry is replaced; content
revision checks supplement that lifetime rule. Label/RGB edits preserve pending
work. Geometry, selection, visibility, locking and document changes reject it.

Saved view record edits continue to dirty the document and enter history without
invalidating geometry. Analysis display settings and marker styles have separate
revision domains. The main loop also retains scene bounds between relevant edits
instead of unconditionally recalculating them every frame.

## Measurement method

Measured on 4 October 2026 on Windows, AMD Ryzen 7 5800H, Visual Studio 2026
Debug (`vs2026-vcpkg`), against the legacy helpers retained from parent commit
`7bed51da6a8e5bb0c702fb8ad8fed48bc6b26b6b`.

The opt-in `scene query cache benchmark` compares the retained legacy query
functions with warmed cache paths in the same executable. It isolates query work;
it is not a before/after application frame-rate measurement. Fixtures contain
256, 4,096 and 16,384 parts with shared triangle vertices and UV coordinates, plus
a separate annotation containing 20,000 surface segments. Each timing is the
median per-call time of seven batches. Batch sizes target 5 ms, capped at
1–1,000 calls per batch; fixture creation and cache warm-up are excluded.
Part and annotation world-line baselines reuse warmed scratch buffers, as the
old renderer did. The projection baseline forces the extracted tessellation
routine to rebuild with warmed working buffers; the cached case retains its
output. This row isolates projection reuse rather than comparing two renderers.

Allocation counts use the existing per-thread MSVC Debug CRT hook, in separate
untimed calls. They include Debug STL bookkeeping and exclude driver/other-thread
allocations. Retained memory is an estimate from container capacities and element
sizes, including ancestor arrays and comparison tree/name storage where relevant.
It excludes allocator overhead, control bytes, unrelated runtime caches and
short-lived consumer scratch. Small-string capacity may already be inline in
the containing object. The selection row reports its inline snapshot size;
selected-ID storage and shared part metadata are excluded from that row. These
estimates are neither process RSS nor an additive total for the application.

## Results

The table shows the range of the two run medians. Part-based rows use 16,384
parts; annotation rows use one source part and 20,000 segments. Raw results also
include 256 and 4,096 parts: [first run](benchmarks/scene-queries-debug.csv) and
[repeat](benchmarks/scene-queries-debug-repeat.csv).

| Query | Legacy/rebuild time (ms) | Cached time (microseconds) | Allocations before → after | Retained cache payload |
| --- | ---: | ---: | ---: | ---: |
| All membership badges | 951.442–1,049.330 | 5,435.7–5,547.0 | 0 → 0 | 240 KiB |
| Root total/visible counts | 1.376–1.864 | 0.413–0.770 | 0 → 0 | 720 KiB |
| Resolve visible parts | 73.916–83.350 | 3,656.7–4,207.7 | 0 → 0 | 5.92 MiB |
| Selection framing and dimensions | 213.829–254.547 | 0.410–0.419 | 1 → 0 | 392 B |
| Hover scene/query signature | 15.434–21.404 | 0.827–1.458 | 0 → 0 | 72 B |
| UV geometry signature | 137.124–151.847 | 0.309–0.331 | 32 → 0 | 2.18 MiB |
| Annotation world lines | 25.316–32.240 | 0.086–0.135 | 0 → 0 | 469 KiB |
| Annotation projection | 68.488–87.636 | 0.793–0.982 | 0 → 0 | 1.37 MiB |

Membership still performs one hash lookup per displayed badge, and resolving
parts still copies short-lived views. Neither path repeats hierarchy traversal
or transform composition on a hit. Selection, annotation and signature hits
retain the derived output. Source metadata and projected annotation vertices
account for most of the added retained memory; no full source meshes are copied
for these query caches.

All warmed cache paths allocated zero times in both runs. The CSV `builds`
column records the warm-up build count. Regression tests separately verify that
unchanged query keys do not increase counters, and that camera-only annotation
reprojection reuses its working buffers without allocating.

These are Debug CPU microbenchmarks, not Release timings or FPS measurements.
No other work from this task ran alongside them, but unrelated build/test
activity was observed on the shared machine. There was no CPU affinity,
frequency or thermal control. The timing ranges are illustrative; the cache
reuse, allocation counts and retained payload are the stronger evidence.

## Validation

- Clean Debug build and final incremental build passed without compiler warnings.
- All 951 CTest checks passed: 950 together, then the updater smoke test in
  isolation. That slow test initially hit its 180-second limit during concurrent
  compiler activity; the isolated rerun passed in 74.34 seconds. The two slow
  unit tests and the build-graph integration test also passed.
- The annotation render smoke test passed for both up axes, lines/rectangles,
  near-plane clipping, distant/inside-bounds views and close occluders.
- Both benchmark runs passed their 24 result-equivalence assertions.

## Reproduction

Reproduce from the repository root:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg --output-on-failure
# Include the four tests labeled slow as well:
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2
& build/vs2026-vcpkg/bin/Debug/woby_tests.exe '--test-case=scene query cache benchmark' --no-skip=true --no-colors=true
uv run tests/ctl_annotation_render_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe
```

The regression tests compare cached results to the legacy paths, assert build
counter stability and zero allocation on warmed queries, and exercise imported
lines/points, inherited transforms, render modes, selection, comparison publication,
annotation reshaping, mesh relocation/replacement, New/Open, undo/redo, reused IDs,
GPU resource changes and rejection of late annotation results.
