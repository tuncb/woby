# Bear Trap interaction after loading

Investigation date: 14 October 2026. Baseline application source: `b503d4c`.
The working tree now includes tested candidates for cached dimensions and spatial
picking. The baseline results below show the original costs. The solution results
show the same model with those candidates enabled.

The reported selection sequence has two blocking CPU operations. Selecting the
model in the Scene list runs an exact dimension calculation. Clicking empty
viewport space to clear selection first runs primitive picking. Both operations
run on the main thread after loading and annotation preparation have finished.

## CPU results

Input: `D:\temp\obj_tests\20190810_BearTrap_Ground_Model.obj`, 7,108,991,827 bytes,
38,414,391 render vertices, and 75,771,986 triangles. The probe uses the production
selection, query, inspector, and picking functions. Its view is 1280 × 720 with
the fitted camera. All required geometry and annotation caches are ready.

Each row below shows three samples in one Release process. Times are milliseconds.
They exclude OS input dispatch and frame presentation.

| Operation | Minimum | Median | Maximum |
| --- | ---: | ---: | ---: |
| Refresh queries after selecting the file | 2143.79 | 2176.78 | 2180.51 |
| Refresh queries after selecting its group | 2159.42 | 2159.48 | 2164.79 |
| Refresh unchanged selected-file queries | 0.0001 | 0.0001 | 0.0002 |
| Refresh queries after clearing file selection | 0.0016 | 0.0019 | 0.0049 |
| Solid only: click model | 16.48 | 16.56 | 17.09 |
| Solid only: click empty corner | 0.0019 | 0.0020 | 0.0022 |
| Triangle edges only: click model | 9794.87 | 9832.76 | 9835.75 |
| Triangle edges only: click empty corner | 8079.58 | 8138.63 | 8159.98 |
| Vertices only: click model | 4757.62 | 4782.67 | 4812.14 |
| Vertices only: click empty corner | 4743.66 | 4752.33 | 4954.56 |
| Solid, edges, and vertices: click model | 12734.45 | 13074.55 | 13200.50 |
| Solid, edges, and vertices: click empty corner | 11034.94 | 11077.75 | 11267.80 |

Changing the selected ID itself takes less than 0.02 ms. The inspector update
after the shared queries have finished takes less than 0.05 ms. Thus, the large
Scene list selection delay comes from the shared geometry query. The clear
operation itself takes less than 0.001 ms. In the user's sequence, the viewport
pick must finish before this clear operation can run.

All center picks returned the expected group ID. All corner picks returned zero.
Logical removal in the CPU probe took 119.10 ms and left no files in the scene.

## Causes

In [`updateSceneSelectionQueries`](../src/scene_queries.cpp), a selection change
invalidates the dimension result. The function calls
[`sceneDimensions`](../src/scene_dimensions.cpp), which visits every referenced
triangle corner in the selected part. This model has 227,315,958 such references.
It reads coordinates and computes exact extents for each one. Selecting again
repeats this work. Only consecutive unchanged frames reuse the result.

The Scene toolbar and Properties inspector both request the shared selection
queries. Hiding dimension labels does not remove all requests for this calculation.
The inspector also uses dimensions for its Size display.

In [`pickSceneObject`](../src/scene_pick.cpp), annotation blocks accelerate the
surface-only path. Enabling edges or vertices disables that path. The fallback
visits every triangle, projects its corners, and tests the displayed edges or
vertices. The part bounds only reject surface ray tests. They do not reject this
edge/vertex traversal when the pointer is outside the entire model.

The mouse-release handler in [`main.cpp`](../src/main.cpp) calls picking before
`clearSceneSelection`. Thus an empty-space click can block the event loop for
about eleven seconds in combined mode. Drawing can be fast while this click
operation remains slow.

## Rendering and removal

The desktop test uses the Release `woby.exe`, NoGraphicsAPI Vulkan, 4× MSAA, and
an NVIDIA GeForce RTX 3070 Laptop GPU with 8 GiB of VRAM and driver 616.92. The
CPU is an AMD Ryzen 7 5800H. The machine has about 64 GiB of system RAM. Each mode has three two-second frame
windows in one process after warm-up. Frame captures were complete, and exported
PNGs were decoded and checked. The combined-mode capture was also inspected.

At 1280 × 720, with adaptive points enabled and X-ray edges disabled:

| Mode | FPS range |
| --- | ---: |
| Solid | 61.54–62.98 |
| Triangle edges | 31.95–32.29 |
| Vertices | 329.79–329.98 |
| Combined | 60.55–61.23 |

Removal through `model.remove` took 153 ms in combined mode and 193 ms after a
fresh solid-mode load. Both commands completed, the resulting scenes were empty,
and the viewer exited normally. Peak process private memory was 10.98 GiB.
This run did not reproduce a separate unload deadlock.

A second process used a 3840 × 2160 drawable, adaptive points disabled, and
X-ray edges enabled. Solid rendering reached 60.29–60.53 FPS, triangle edges
9.39–9.41 FPS, vertices 236.03–240.02 FPS, and combined display 8.16–8.25 FPS.
Combined-mode removal took 285 ms; removal after a fresh solid-mode load took
201 ms. Both scenes became empty and the viewer exited normally. These settings
make drawing much slower, but this test also did not reproduce an unload deadlock.

The render/removal test uses the API and has no selected source object. It does
not reproduce the complete mouse/menu sequence. The CPU probe measures the
selection functions separately. Three windows or samples in one process do not
establish variation between independent processes.

## Tested solutions

The target is less than 50 ms of interaction work after preparation, including
the first selection. Visible feedback also needs a completed display frame.

| Operation on Bear Trap | Baseline median ms | Candidate median ms |
| --- | ---: | ---: |
| Refresh queries after selecting the file | 2176.78 | 0.0031 |
| Refresh queries after selecting its group | 2159.48 | 0.0010 |
| Combined mode, click empty corner | 11077.75 | 0.0037 |
| Combined mode, click model center | 13074.55 | 33.75 |
| Solid mode, click model center | 16.56 | 0.27 |

The first selected-file query took 0.0102 ms. This fast path does not depend on
having selected the model before. All eight center/corner results matched the
baseline IDs. An additional 9 × 9 pointer grid contained 78 empty points and three
hits. The slowest empty-point query took 0.0206 ms; the slowest hit took 34.0023 ms.
Two grid points were also checked with exhaustive picking. Their object IDs,
triangle IDs, and barycentric coordinates matched exactly.

### Prepare exact local dimensions once

Store exact local extents for each triangle part in the immutable annotation
cache. Build them on the preparation worker. Selecting one prepared part then
copies six doubles and applies its transform. Geometry edits invalidate the cache
through the existing geometry identity checks.

The cache uses the original local double coordinates. It does not convert absolute
original bounds back to local coordinates, which could lose small dimensions at
large origins. Tests cover precise coordinates, unused vertices, partial ranges,
and invalidation after edits. This model needs 56 bytes for its cached extent.
Annotation preparation increased from 3434 ms to 5576 ms in the CPU probe. The
additional preparation replaces later blocking scans. Both stages run before the
model is ready for the measured interactions.

The current fast dimension path covers a single prepared triangle part, including
its file, group, or parent selection when only that part contributes. Bear Trap has
one part. Multi-part selections retain the existing exact world-coordinate scan.
For those selections, cache transformed exact extents per part or use an exact
support query over a geometry hierarchy. Do not substitute transformed local boxes
for exact rotated extents.

### Query the existing spatial hierarchy

Use the annotation hierarchy for surface, edge, and vertex picking. Test its boxes
against the pointer footprint in clip coordinates. Reject boxes outside that
footprint before examining triangles. Include edge tolerance, marker radius,
near/far clipping, and the positive projection denominator. This test stays
conservative when a box crosses the near plane.

At the combined-mode center point, the candidate examines 162,393 triangles rather
than all 75,771,986. At the empty corner, it tests one hierarchy node and no
triangles. Original triangle offsets are sorted before primitive tests so equal
depths retain the original source order and UV triangle identity. The normal
occlusion, transparency, and X-ray rules still apply.

The comparison tests check all seven primitive combinations, two opacity/X-ray
settings, three projections, transforms, large markers, partial depth clipping,
and 81 pointer positions per combination. They compare IDs and exact hit details
with exhaustive traversal. A separate test checks that an empty footprint visits
no triangles. Geometry without a current spatial cache keeps the existing fallback.

### Keep memory release outside the interaction

A CPU retirement experiment moves the removed mesh into an owned snapshot before
removing its logical scene entry. Detaching the snapshot took 0.0002 ms; logical
removal took 0.0083 ms. Starting the release worker took 0.0499 ms. Releasing the
geometry took 117.93 ms on that worker. A weak ownership check confirmed that the
scene was empty before release and that the geometry was released after completion.

This retirement experiment is in the probe. The viewer's removal path is unchanged.
A viewer implementation should use an existing persistent worker or a bounded
release queue. Move CPU geometry and point-cloud owners there, while keeping GPU
handle destruction on the graphics thread and retaining the existing GPU fences.
Do not join the worker in the model-removal handler. The measured viewer removal
still takes about 0.16 seconds.

### Alternatives

| Approach | Assessment for this requirement |
| --- | --- |
| Cache only the last selection result | Leaves the first selection and selection after clearing slow. |
| Add only a whole-model bounding-box check | Helps far-away misses, but leaves holes and near-model clicks expensive. |
| Run the existing full scan on a worker | Keeps the event loop active, but still delays selection by seconds. |
| Extend GPU IDs to select objects | Possible, but existing IDs identify vertex markers. Surface/analysis IDs, tolerance, transparency, and stale-frame handling need more work. Readback also follows GPU frame time, which exceeds 50 ms in the heavy X-ray control. |
| Cached exact dimensions plus the existing hierarchy | Meets the target on this model and preserves the tested pick results. |

## Desktop selection check

The normal Release viewer loaded the model and a temporary scene with Selected
and Cleared saved views. Applying these views changes selection through the normal
scene state, then runs the normal Scene and Properties UI. This check uses API
commands rather than injected mouse events.

At 1920 × 1080 with combined rendering and both panes open, three select/clear
cycles produced no selection stall. The maximum UI-build time was 0.621 ms. The
slowest completed frame across those windows took 22.157 ms. Removing the selected
model in combined mode took 163 ms; the scene became empty and the viewer exited
normally. Initial model loading took 13.13 seconds in this run, compared with
11.07 seconds in the earlier desktop baseline. These are separate runs, not a
controlled estimate of a small loading-time difference.

The interaction fixes do not change geometry rendering throughput. Visible feedback
still follows frame completion; the earlier 8 FPS X-ray control cannot show a new
frame every 50 ms. The fixes remove the separate multi-second CPU stalls.

The [probe instructions](../experiments/post-load-interaction/README.md) contain
the build and reproduction commands. Recorded data is under
[`benchmarks/post-load-interaction-20261014`](benchmarks/post-load-interaction-20261014).

## Validation

The complete Release build with the `vs2026-vcpkg` preset passed without warnings.
The final probe build also passed without warnings. The standard Release CPU
suite passed all 113 CTest entries, including the new probe contract. The contract
checks selection dimensions, unchanged-query reuse, selection clearing, hits and
misses in each display mode, and model removal on an in-memory triangle fixture.
No timing threshold is used as a correctness test.
The candidate Release application and tests also built without warnings, and all
113 standard CTest entries passed with the new dimension and spatial-picking cases.
