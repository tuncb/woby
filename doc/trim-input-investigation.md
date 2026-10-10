# CDT input and trim representation

Investigation of items 9a and 9b on 11 October 2026, based on `f370d2e`.
Implement the iterator input change first. Keep its batch sizes and cancellation
points. Then test a cell-based trim representation in a separate prototype.
The latter can remove much more triangulation work, but it changes connectivity.
9a is now implemented in production with the same batch sizes and getters. The
user has excluded backward compatibility from 9b: new triangulation does not need
to preserve old annotation attachments. The measurements below describe the
investigation before the production change.

## Current work

[`triangulateFreeformTrim`](../src/freeform_trim.cpp) samples rational trim curves,
validates loops, and crops the grid to the union of outer-loop bounds. It builds
vertices and constraints for that rectangle, appends the trim boundaries, and
removes duplicate vertices. CDT then inserts vertices in batches of 1,024 and
edges in batches of 256. Each call currently receives a new temporary vector.

Every horizontal and vertical grid edge is constrained. This keeps surface knot
lines and connects neighboring cells. CDT resolves intersections between those
edges and the trim boundaries. Woby removes the supertriangle, tests each triangle
center against the trim regions, and compacts retained vertices in triangle order.
The output is the existing `FreeformGrid.samples` and `triangles` representation.

The earlier [bounds crop](freeform-loading-performance.md) is already part of this
baseline. Its speedup must not be attributed to either new proposal. These changes
affect native trimmed freeform import and reconstruction. They do not accelerate
ordinary polygon meshes or steady-state rendering of an already loaded mesh.

## 9a Iterator input

The installed CDT version is 1.4.4. Its vector overloads delegate to iterator
overloads with `getX_V2d`, `getY_V2d`, `edge_get_v1`, and `edge_get_v2`. Supply the
same iterator ranges and getters directly. Keep duplicate removal, insertion
order, error handling, capacity checks, and the progress callback before each
batch. This removes one allocation and copy per batch. CDT still copies vertices
into its own storage. The full input vectors remain necessary in this change.

The [probe](../experiments/trim-input/main.cpp) reproduces those input batches on
15 synthetic cases. Each case checks exact coordinate values, triangle vertex
order, triangle neighbors, fixed edges, and checkpoint counts. All comparisons
passed. This is direct CDT equivalence evidence. A production change must also
run the existing trim tests and compare final mesh indices and saved fingerprints.

Seven measured pairs follow one warm pair, with alternating execution order.
These Release times include CDT insertion and supertriangle removal. They exclude
input preparation, trim-curve sampling, validation, containment filtering, mesh
evaluation, and rendering. They are not whole-model load times.

| Fixture, 256 cells per domain axis | Copied batches median ms | Iterators median ms | Copy traffic removed | Allocations removed |
| --- | ---: | ---: | ---: | ---: |
| Full rectangle | 510.06 | 479.44 | 2,109,488 bytes | 580 |
| Diagonal strip | 447.50 | 442.25 | 1,948,432 bytes | 535 |
| Annulus | 466.46 | 434.56 | 1,972,912 bytes | 540 |
| Small rectangle after crop | 2.76 | 2.70 | 22,992 bytes | 7 |
| Full rectangle with a tiny hole | 511.65 | 547.83 | 2,109,584 bytes | 580 |

Across all grid sizes, median changes range from about 13% faster to 9% slower.
This run does not establish a consistent speedup. The reliable benefit is removal
of unnecessary allocations and copy traffic. Peak temporary payload falls by at
most 16 KiB for these batches, excluding allocator overhead. The table's cumulative
copy bytes are not a peak-memory saving.

Do not combine batches as part of 9a. CDT `Auto` uses a KD-tree traversal for the
first call and randomized insertion for later calls. Changing batch boundaries
also changes initial bounds, allocation behavior, and cancellation opportunities.
Keeping batches preserves cancellation opportunities; it does not put a new time
limit on work inside one CDT call.

## 9b Cells with explicit trim boundaries

Keep the sampled and validated loops and the current cropped grid. Divide its
cells into three sets. Emit two triangles directly for a retained interior cell.
Omit an exterior cell. Clip and triangulate cells that contain or touch a boundary.
The final result can still use `FreeformGrid.samples` and `triangles`; the new
representation can remain local to tessellation.

The probe counts these sets after the existing crop. Boundary tests use closed
cell boxes, so a segment on a shared grid edge marks both neighboring cells.
These counts describe potential work reduction, not a completed replacement
triangulator or a measured 9b speedup.

| Fixture, 256 cells per domain axis | Cropped cells | Interior | Exterior | Boundary | Boundary share |
| --- | ---: | ---: | ---: | ---: | ---: |
| Full rectangle | 65,536 | 64,516 | 0 | 1,020 | 1.6% |
| Diagonal strip | 60,516 | 2,156 | 57,380 | 980 | 1.6% |
| Annulus | 60,516 | 6,608 | 52,028 | 1,880 | 3.1% |
| Small rectangle after crop | 676 | 576 | 0 | 100 | 14.8% |
| Full rectangle with a tiny hole | 65,536 | 64,515 | 0 | 1,021 | 1.6% |

The diagonal and annular cases have the largest opportunity to discard work.
The full rectangle can bypass CDT for most cells, but it must still allocate,
emit, and evaluate nearly all retained geometry. The small rectangle already
takes only about 3 ms in CDT after cropping; local triangulator setup may erase
its remaining gain.

The probe uses a simple scan of boundary segments for each cell. On the annulus,
that classification alone took 645 ms, compared with 435 ms for iterator-based
global CDT. Thus a naive cell implementation can be slower. Build a segment-to-cell
index or traverse each segment through the nonuniform grid. Use row crossings or
region spans for containment. Measure the cost of this index and the local CDT
instances as part of the replacement, not outside its timing.

## Required properties and costs

| Area | Required behavior and cost |
| --- | --- |
| Boundary sampling | Reuse the existing rational curve sampling and spatial and UV tolerances. A 512-segment polygonal circle in this probe is not a validation of rational sampling. |
| Small features | A loop or hole can lie entirely inside one cell. Register its segments even when no grid edge is crossed. Corners or cell centers alone are insufficient. |
| Shared vertices | Compute each grid-edge intersection once. Use stable keys for grid vertices, boundary vertices, and edge intersections. Neighboring cells must use the same coordinates and vertex IDs. Independent rounding or epsilon welding can create cracks or remove narrow features. |
| Contacts | Handle vertices on grid corners, segments along grid edges, and almost coincident intersections. Preserve current rejection of touching or intersecting trim loops. Valid contact with a grid line is a different case. |
| Domains and knots | Use actual `grid.u` and `grid.v`, including nonuniform knot intervals and nonunit parameter domains. No emitted triangle may cross a constrained knot line. |
| Regions and holes | Keep region identity and hole ownership. A cell can contain several disconnected retained pieces or a hole. A single polygon clip or fan cannot cover all cases. |
| Connectivity | Local CDT and a fixed interior diagonal can produce a different triangulation from global CDT. On a curved surface, the other diagonal also changes the piecewise planar surface, so area agreement in UV is not enough. |
| Determinism | Fix cell traversal, intersection order, local constraint order, interior diagonal choice, and final vertex compaction. Determinism across new runs does not mean equality with old output. |
| Cancellation | Check progress during index construction, cell traversal, local triangulation, and assembly. Bound local work and retained input sizes. Keep worker snapshots owned until completion. |
| Memory | A boundary index, shared intersection registry, and output assembly need storage. Avoid a dense full-domain cell array when the retained region is sparse. Report peak memory as well as time. |

Do not substitute `eraseOuterTrianglesAndHoles` in the current global graph.
Internal grid constraints and trim boundaries are both fixed edges; CDT's layer
classification does not know which edges have trim semantics. A cell method needs
explicit region classification and local boundary roles.

## Saved annotation compatibility

[`annotationFingerprint`](../src/surface_annotation.cpp) hashes the ordered mesh
indices and float positions. Saved annotations also retain triangle references.
[`validAttachments`](../src/ui_annotation_operations.cpp) rejects a changed
fingerprint. A geometrically equivalent mesh with different IDs or triangle order
can therefore leave annotations unresolved after a model is loaded again.

9a should preserve the old mesh with the same input order and batches. 9b cannot
make that promise merely by sorting its output: diagonal choices and intersection
coordinates can differ. The user has explicitly excluded backward compatibility
from 9b. No legacy tessellation path or annotation migration is required. Existing
fingerprint validation must continue to reject changed attachments; do not silently
attach an old triangle reference to a different triangle.

## Recommended order

1. Implement 9a as a small independent change. Keep the exact batch boundaries.
   Verify final samples, mesh indices, source IDs, annotation fingerprints,
   error diagnostics, and cancellation against the old path.
2. Add a prototype boundary index and cell classifier using the existing sampled
   loops. Compare it with a slow reference. Include a hole and an entire region
   inside one cell, exact grid contacts, narrow regions, and nonuniform grids.
3. Add a shared intersection registry and a local triangulator for boundary cells.
   Keep the current global path as the reference. Check retained area, positive
   winding, knot constraints, edge incidence, cracks, and geometric error on
   curved and rational surfaces. Test multiple regions and reversed loops.
4. Benchmark full trimmed import, peak memory, and cancellation on real keycaps and synthetic
   diagonal and annular trims. Include small patches to detect setup regressions.
   Adopt 9b only if its total cost and compatibility behavior meet those checks.

9a remains useful inside a future local CDT path, but 9b does not depend on it.
Keep their changes and performance claims separate so each result is reviewable.

## Reproduction and evidence

The [raw results](trim-input-results.json) contain every measured total time and
the counts for 32, 128, and 256 cells per domain axis. The fixtures are synthetic
polylines. The annulus uses radii 0.48 and 0.44, with 512 segments per circle.
The run used Windows, Ryzen 7 5800H, Visual Studio 2026, CDT 1.4.4, and the Release
configuration of `vs2026-vcpkg`. Builds and unit tests did not run during timing.

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_trim_input_probe
build/vs2026-vcpkg/bin/Release/woby_trim_input_probe.exe
```

The opt-in probe's unit check compares both CDT paths and checks cell accounting,
grid-aligned boundaries, a hole inside one cell, corner contact, and disjoint
segments. Production trim tests already cover rational holes, multiple regions,
invalid boundaries, narrow features, cropping, nonunit domains, determinism,
and cancellation. The full Debug build and Release probe build completed without
warnings. All 81 unit test groups passed in 58.71 seconds, including the new probe
and the existing trim tests. `git diff --check` passed. Logs are in
`build/trim-input-configure.log`, `build/trim-input-debug.log`,
`build/trim-input-release.log`, and `build/trim-input-tests.log`.

## 9a implementation

Production now passes iterator ranges directly to CDT. Batches, getters,
constraint order, cancellation callbacks, and capacity checks are unchanged.
The copied-input comparison remains in the opt-in probe as a reference. An added
production test injects cancellation at every reported checkpoint in a trimmed
33 by 33 grid, including insertion and output filtering.

The implementation passed the full Debug build without warnings and all 81 unit
test groups in 54.36 seconds. Logs: `build/trim-9a-debug.log` and
`build/trim-9a-tests.log`. The probe's exact CDT comparisons passed in that suite.
