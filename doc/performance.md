# Performance engineering

Measured on 13 September 2026. This pass changes import, GPU preparation, scene
queries, and CPU analysis. The measurements below come from executable workloads
and a real viewer; the later architecture proposals are not implemented.

## Results

CPU comparisons use the same standalone benchmark driver, MSVC Release settings,
and machine. The baseline executable was preserved from the source at
`9906867615defff458efe26c40dbee3c38be4d6c` before modifying the core algorithms.
The machine has a Ryzen 7 5800H (8 cores / 16 hardware threads), approximately
64 GiB RAM, and an RTX 3070 Laptop GPU. Builds use Visual Studio 2026 and the
`vs2026-vcpkg` preset. Baseline and updated runs execute sequentially, without
another benchmark, viewer, build, or test suite running alongside them.

| Workload | Geometry | Baseline median | Updated median | Speedup |
| --- | ---: | ---: | ---: | ---: |
| OBJ import, 5 repetitions | 320,000 triangles | 117.201 ms | 77.618 ms | 1.51x |
| Vertex compaction, 5 repetitions | 1,280,000 triangles | 104.514 ms | 82.924 ms | 1.26x |
| Bidirectional distance stage, 3 repetitions | 80,000 triangles per side | 1,169.170 ms | 414.954 ms | 2.82x |
| Bidirectional distance stage, 3 repetitions | 999,698 triangles per side | 18,160.000 ms | 5,999.550 ms | 3.03x |
| Surface quality, 5 repetitions | 80,000 triangles | 118.110 ms | 52.274 ms | 2.26x |

Distance timings include both BVH builds, all four samples per triangle in both
directions, expanded heatmap geometry, bounds, weighted means, and weighted P95.
They exclude topology/duplicate detection, world-transform assembly, and GPU
upload. OBJ timing includes parsing and mesh construction but excludes writing
the fixture. Compaction excludes copying the input fixture. These are synthetic
regular grids, including two parallel surfaces separated by 0.25 for distance;
they do not establish equivalent speedups for arbitrary CAD geometry. Repetitions
include the first run, with no deliberate page-cache flush or thermal control.

The separate real-app workload imported **2,500 distinct OBJ files in 6.30 s**,
with no skipped or failed files. Each file contains one triangle, so this tests
object-count scaling, not large-geometry throughput. With the Objects pane
visible, 20 `performance get` observations gave a **13.75 ms median frame time**.
Median stage observations included 9.67 ms for UI construction, 1.47 ms for scene
state, 1.43 ms for scene submission, and 0.74 ms for hover picking. The script
also toggled edge/point modes, captured the view, saved, cleared, reopened, and
verified all 2,500 files. Captures were visually inspected.

An intermediate profile with the Objects pane open exposed approximately
245 ms of UI construction per frame. The final local file-availability query
reduced that to 9.67 ms. This is an intermediate-to-final comparison, not a
measurement of the untouched application's frame time. Similarly, intermediate
serial import runs varied from roughly 15 to 26 seconds; they are useful evidence
for investigating the loader, not a controlled cold-cache speedup claim.

## Implemented changes

### Import and compaction

`obj_mesh.cpp` replaces the node-allocating vertex hash map with dense keys and
32-bit open-addressed buckets. Position, normal, and UV indices remain part of
the key, preserving seams and the first-use vertex order. The table grows before
its load factor exceeds 75%. Source-position and source-index arrays are reserved
up front. Count validation prevents unsupported 32-bit index ranges.

`compactMesh` remaps indices in place and removes a redundant vertex-fetch pass
and its full-sized copy. The installed meshoptimizer remapper already assigns
first-index-use vertex IDs. Triangle order remains unchanged, preserving face
annotations and source provenance. The remap array is now sized by source vertex
count, fixing an out-of-bounds case when there are more source vertices than
indices. There is no mesh simplification in this change.

For Windows OBJ files up to 1 MiB, parsing uses a buffered stream and an explicit
adjacent material-library search path. Larger files retain RapidOBJ's native
parallel reader. The bundled Windows native reader uses unbuffered I/O, which is
a poor fit for repeated tiny files. The isolated warm two-triangle benchmark
showed only a modest improvement from buffering, so buffering alone should not
be credited with the whole batch-loading improvement.

For batches of at least eight files, `background_load.cpp` keeps at most four
small built-in OBJ/STL loads in flight. Hardware with fewer available cores falls
back to fewer workers or serial execution. Large inputs and plugin imports form
serial boundaries, avoiding nested large-parser fan-out and preserving plugin
callbacks. The coordinator consumes futures in input order: colors, per-file
outcomes, progress callbacks, and scene settings retain their prior order.
Cancellation publishes only completed entries; destruction joins the bounded
look-ahead work. The 1 MiB threshold bounds input size, not worst-case decoded
memory or elapsed parsing time.

### GPU preparation and frame scheduling

The default solid mesh now allocates one vertex buffer and one index buffer.
Edge indices and expanded point sprites are created when visible display modes
request them, then retained for subsequent toggles. Compact CPU point lists stay
available immediately for picking and geometry tooltips. Optional staging vectors
are handed to bgfx with an ownership callback, avoiding another staging copy.
Allocation failures retain already-valid display buffers where possible, and a
failed optional upload is not retried every frame until the requested modes change.

For `V` render vertices, `T` triangles, and `P` per-group unique point references,
the previous buffers required approximately `32V + 36T + 104P` bytes. Solid-only
preparation now requires `32V + 12T` GPU bytes; the `4P` CPU point-index array is
retained. For a regular mesh with roughly two triangles per vertex and one group,
that is about **73% less initial geometry-buffer data**, excluding driver overhead,
transient upload memory, allocator rounding, and CPU mesh/source storage.

GPU finalization now consumes a **4 ms soft budget per frame**, allowing multiple
small files per frame instead of imposing one frame per file. A single large
allocation can exceed this budget. A batch can report multiple GPU failures in
one frame without losing the individual automation outcomes.

### Scene and UI queries

File-row Analysis availability checks only the file's own valid identified parts.
The former implementation expanded an object selection by walking the entire
scene for each file row, producing quadratic work with thousands of files.

Bulk comparison membership, counts, tree construction, world-mesh traversal,
and missing-reference checks now build sorted ID sets and use binary search.
Adding/removing thousands of comparison members and resolving their names uses
one lookup construction per operation. Source grouping and automation append
outcomes similarly resolve file IDs/paths once. Scene saving and history snapshots
build a part-location lookup once rather than rescanning every file for every
comparison member. Missing-reference fallback behavior and serialized ordering
remain intact. These are operation-local structures, with logical state still
owned by `UiState`.

### Distance analysis

The distance tree has four children per node, 64-byte alignment, and contiguous
double-precision structure-of-arrays bounds. Its 256-byte nodes evaluate the four
child bounds as two SSE2 lane pairs on x64; other architectures retain a scalar
path. No global AVX requirement or float narrowing is introduced.

Balanced median partitions limit leaf size to eight triangles. The private
triangle array is rearranged in place into leaf order, then the permutation array
is released. Near-first iterative traversal uses a fixed stack: 32-bit primitive
counts and median splits bound the depth, with room for three deferred siblings
per level. Each sample uses the preceding sample's nearest triangle as an exact
initial upper bound, improving pruning for spatially adjacent queries.

Sample generation writes disjoint, pre-sized output ranges using a dynamic batch
queue. Simultaneous analyses share a budget of at most eight auxiliary workers,
reduced on smaller CPUs to leave capacity for UI/render/coordinator work. Small
jobs execute serially. Worker exceptions propagate after joining; thread-creation
resource exhaustion falls back to the coordinator. Output initialization and long
loops check cancellation in bounded batches.

Reduction order stays serial and fixed, so weighted means and P95 do not depend
on worker scheduling. Samples remain in original face order, with the existing
double-precision point-to-triangle calculation and four-way subdivision. SIMD
accelerates pruning; it does not alter analysis sampling or silently replace
exact input geometry with a rendering approximation.

### Surface quality

Quality inspection caches welded IDs per input vertex, allocates its geometric
welding map from a monotonic arena, and replaces edge-tree insertions with a
contiguous edge array followed by sort/reduce. Boundary and nonmanifold edges
retain their previous neighbor rules. The geometric map still merges normal/UV
seams and signed zero, and degenerate faces retain their prior treatment.

## Validation and reproduction

Debug and Release builds completed with no compiler warnings. **All 470 CTest
entries passed**, including all four slow tests; the full run took 84.01 seconds.
Coverage includes sparse vertex compaction, hash growth and seams, Unicode OBJ
paths, small/large parser parity, neighboring material libraries, thousands of
comparison members and saved references, exhaustive irregular distance queries,
deterministic repeated parallel results, cancellation, worker failure recovery,
quality welding/nonmanifold cases, and lazy GPU buffer creation/reuse under bgfx's
Noop backend. Prefetch tests cross the four-slot reuse boundary with duplicate
paths, malformed inputs, skips, persisted settings, and cancellation; callbacks
are checked on the coordinator thread. New filesystem fixtures use one unique
temporary directory per test and clean up on failure.

The frame-budget and automation wiring inside `main.cpp` are covered by the
real-viewer workflow rather than isolated unit tests: isolating that anonymous
adapter state would require implementation restructuring. Real-app controls smoke
also exercises persistence, importer callbacks, retries, failures, and screenshots.
One final controls-smoke attempt received empty output from instance discovery
during startup and failed JSON decoding; an immediate rerun passed. That startup
transient was not reproduced or diagnosed by this performance pass.
The SSE2 branch was executed on this machine; the portable scalar branch was not
executed here.

From the repository root, with `VCPKG_ROOT` set to the local vcpkg checkout:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --parallel 2 --output-on-failure
cmake --build build/vs2026-vcpkg --config Release --target woby woby_benchmarks --parallel 2
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe obj 400 5
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe compact 800 5
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe distance 200 3
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe quality 200 5
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe distance 707 3
python tests/performance_smoke.py build/vs2026-vcpkg/bin/Release/woby.exe build/performance-qa 2500
```

Benchmarks are optional and excluded from ordinary tests. Their CSV includes
minimum/median/maximum times and a checksum, with no flaky timing assertions.
The viewer workload retains JSON observations, stage logs, and PNG captures in
the specified output folder; source/scene fixtures are temporary. The committed
`performance-results.csv` records the core benchmark observations from this pass.
The viewer clears the scene before reopening to avoid doubling GPU handles during
transactional replacement. CI continues to use its existing Ninja/vcpkg workflow.

## Next architecture work, in priority order

These proposals follow from the inspected code and measured bottlenecks. They
require separate implementation and validation; their speedups are not measured.

### 1. Shared GPU pages and submission compaction

The local bgfx build allows 4,096 vertex-buffer and 4,096 index-buffer handles.
Even tiny models can exhaust those pools; solid-only preparation postpones the
limit but does not remove it. Enabling all overlays consumes more handles, and
transactional scene replacement can briefly double residency. Raising the pool
alone leaves both memory overhead and submission cost in place.

Store geometry in shared vertex/index pages and give each runtime mesh page IDs
and ranges. Use generation-tagged allocations, fence-delayed reclamation, and
bounded staging rings so background uploads cannot overwrite data still in use.
Batch compatible opaque draws, then add indirect submission and GPU visibility
compaction where the backend supports them. Preserve transparency ordering,
object IDs, per-part visibility, and picking semantics. Add meshlet/frustum
culling and rendering LOD against an explicit frame-time/visual-error target.
Meshoptimizer provides relevant meshlet, simplification, and indexing primitives;
evaluate those against Woby's source-provenance requirements.
[Meshoptimizer documentation](https://github.com/zeux/meshoptimizer).

Acceptance workload: 10,000 and 50,000 parts, all display modes, repeated scene
replacement, and continuous camera movement. Measure handles, resident/upload
bytes, draw count, and frame P50/P95/P99 separately.

### 2. Immutable mesh storage and background world-mesh assembly

`comparison_view.cpp` still calls `comparisonWorldMesh` on the main thread before
launching the analysis worker. `comparison_scene.cpp` expands transformed selected
triangles into new vertices. For millions of triangles, this can cause a large
pause even when the subsequent calculation is parallel.

Introduce immutable shared geometry payloads and snapshot the selected IDs,
transforms, and provenance. Assemble transformed inputs on a worker, publishing
only if the runtime generation still matches. Keep logical controls in `UiState`
and immutable caches/scheduling in runtime structures. Share unchanged geometry
across history snapshots, imports, analyses, and instances. Avoid copying a huge
mesh just to make the background task safe; ownership is the prerequisite.

### 3. Reusable picking acceleration and revision caches

`scene_pick.cpp` still scans candidate triangles and point references on the CPU.
Build a reusable per-mesh BVH and a scene-level hierarchy over transformed
instances. Refit instance bounds after transforms, rebuilding only when needed.
Closest point, ray, and screen-space edge/point selection need different query
rules; preserve pixel tolerance, x-ray behavior, transparency, and draw-order ties.

Cache geometry signatures and flat visible tree rows by scene revision. The
Objects pane still costs 9.67 ms for the measured workload, so virtualizing the
tree to submit only visible rows is a substantial remaining UX opportunity.
Multiple analysis runtimes also still perform repeated scene/signature scans.

### 4. Compact analysis outputs and retained acceleration structures

Distance heatmaps currently expand every source triangle into four triangles:
12 render vertices, 12 indices, four double distances, and four double areas.
At the current sizes this is approximately **496 bytes per source triangle per
side**, before source meshes, BVHs, diagnostics, percentile sort storage, and GPU
copies. One million triangles on each side therefore consumes roughly 946 MiB
just for those CPU arrays. The single-buffer size validation also caps expanded
vertex output at about 11.18 million source triangles per side.

Keep compact source triangle IDs plus sample data and derive display geometry
through shaders or bounded pages. Stream statistics separately from visualization
residency. Preserve full accuracy and source IDs when display LOD changes. Retain
BVHs by immutable geometry/version so distance-stage invalidation does not always
rebuild them. Quantized bounds require outward rounding and conservative error
tests before they are suitable for pruning accurate distance queries.

### 5. Wider CPU kernels and GPU distance queries

Benchmark AVX2/AVX-512 kernels using runtime ISA dispatch, plus an ARM NEON path,
against the current portable interface. Use hardware counters to separate branch
misses, cache misses, memory bandwidth, and arithmetic throughput. Triangle packets,
precomputed edge data, and BVH8 layouts may help, but also increase working sets.
Compare median splits with SAH and Morton/LBVH construction on representative
thin, overlapping, clustered, and degenerate CAD meshes. A more expensive tree
can win for repeated queries; fast construction can win for frequent edits.
[PBRT's BVH discussion](https://www.pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies)
provides the relevant layout/build tradeoffs; Woby's closest-point workload still
needs its own measurements.

For GPU analysis, prototype coherent batches over an explicitly designed
closest-point BVH. Ray-tracing acceleration should not be assumed to supply an
equivalent nearest-surface result automatically. Keep the CPU double-precision
implementation as an oracle; use conservative candidate generation followed by
double-precision refinement where necessary. Test large coordinates, near-zero
separations, slivers, ties, and nonmanifold inputs before trusting aggregate
statistics. Include upload, build, synchronization, and readback in timings.
Embree's point-query implementation is a useful independent comparison candidate.
[Embree source and tutorials](https://github.com/RenderKit/embree).

### 6. Persistent binary geometry cache and storage streaming

Repeatedly reading and interpreting thousands of text files remains avoidable
work. Add a content-addressed binary cache keyed by source content, importer ID
and version, import options, and schema version. Store render geometry, exact
source provenance, bounds, and optionally reusable acceleration data in independently
validated chunks. Handle cache corruption, interrupted writes, source replacement,
and importer updates explicitly. Memory-map immutable chunks where appropriate;
choose compression using measured I/O versus decompression costs.

Only then evaluate DirectStorage/GPU decompression for Windows bulk streaming.
DirectStorage 1.4's March 2026 public-preview announcement includes CPU/GPU Zstd
decompression; it requires authored compressed assets and explicit integration.
It does not eliminate OBJ lexical parsing or CPU topology work. Validate the
chosen runtime/driver combination, keep a CPU path, and compare complete scene
readiness and frame stalls rather than raw storage bandwidth.
[Microsoft's DirectStorage 1.4 announcement](https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/).

Use separate corpus buckets for many tiny files, single huge meshes, assemblies
with repeated geometry, heavily seamed OBJ, large binary STL, and plugin imports.
Collect cold and warm loads, time to first useful frame, cancellation latency,
peak committed memory, peak GPU residency, and frame-tail latency. A fast median
alone is insufficient evidence for a smooth interactive experience.
