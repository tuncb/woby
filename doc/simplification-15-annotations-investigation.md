# Item 15: GPU annotation projection

Baseline: `46a27585c5bc8638a8bde5d601e5cead71376631`. The proposal is listed
under “Larger alternatives and limits” in the original assessment. Production
behavior is unchanged by this investigation.

## Decision

Implement and validate the smaller persistent projected-buffer change first.
At 100,000 visible segments, it avoids a measured 0.33 ms memcpy boundary and
7.2 MB of CPU upload/staging bytes per stationary frame, while it retains the exact
CPU projection. A GPU experiment is justified for workflows with tens of
thousands of live segments: camera projection already costs several milliseconds.
The GPU migration is not ready to merge; its precision and frame cost remain
unmeasured. The exact CPU slope cache is useful for repeated faces, but an
unconditional cache regresses unique-face work and needs a selection policy.

## Current work

`updateSceneAnnotationQueries` derives world-space endpoints when geometry,
scene membership/transforms, validity, or visibility changes. Its cache retains
24-byte `DiagnosticEdge` records. `updateAnnotationProjection` independently
caches camera-dependent tessellation. A stationary camera/style/viewport returns
without processing segments. Color changes do not invalidate projection; width,
pixel scale, render projection, view matrix, viewport, and world build do.

Every emitted nondegenerate segment has six 12-byte NDC vertices: 72 bytes.
`submitSceneAnnotations` calls `submitLines` for each visible annotation. That
function allocates a transient vertex buffer and copies the projected array on
every submission, including stationary views. There is no retained GPU projected
buffer. The adapter then retains transient allocation/staging until the frame
finishes. The CPU cache saving and CPU upload saving are separate opportunities.

For a normal surface segment the projection reads the target render triangle's
three float positions, composes the source/model/view/render transform, and finds
the NDC depth-plane slope. Determinant and slope intermediates use double after
the float transforms and NDC quotients. It transforms world endpoints, clips centers against
six homogeneous planes, rejects non-positive w and zero screen length, expands
screen width, adjusts depth by the triangle plane slope, applies relative
reversed-depth bias, and divides to NDC. Bridge segments skip the triangle slope
and can have a second source/triangle endpoint. The final vertex shader passes
those NDC positions through, with the Vulkan Y conversion.

The source triangle slope is essential. Merely thickening a world line uses
the centerline depth on both sides; its uphill side can sink below the surface.
The bias is relative to depth: a fixed NDC bias can bring distant annotations
through occluders. Near-plane center clipping happens before width expansion.
These are output rules, not incidental implementation details.

## Measured boundaries

The probe invokes the unchanged production projection on 10, 1,000, 20,000,
and 100,000 attached segments. One workload reuses 64 source triangles. A second
has one source triangle per segment, to expose cache overhead. Triangle positions
repeat within a controlled normalized view; source IDs remain distinct. Cases
cover flat triangles, oblique depth planes, half bridge segments, and near-plane
crossings. Five alternating samples measure repeated camera rotation, an exact
slope-cache candidate, stationary cache hits, and a standalone memcpy of the
final projected array. It warms projected capacities first.
The source triangles remain within a controlled normalized view. This gives
repeatable workload costs; it is not a claim about user annotation counts.

The standalone memcpy excludes transient allocation, graphics adapter staging,
driver submission, GPU transfer, and rasterization. The camera cost includes
the existing projection and cache checks. The probe does not execute a GPU
candidate, so it cannot establish net frame speed or GPU precision equivalence.
The contract checks cache hits, camera invalidation, visible-segment counts,
finite output, bridges, and near-plane crossing output.

The slope-cache candidate retains exact double arithmetic and changes only reuse
of the source triangle slope within one projection build. It keys by source and
triangle, so independently transformed sources remain distinct. Cache allocation,
lookup, and destruction are included in its timing. A custom allocator reports
requested bytes, excluding allocator overhead and process working set. The full
projected arrays must match production bit for bit after every timed pair. The
contracts also cover transformed multi-source bridges and nearly edge-on views.
Checked extraction anchors and configure dependencies keep the research copy
tied to the current production function.

This is a per-annotation cache. The 64-face workload has high reuse at large
segment counts and is a favorable case, not measured scene usage. A straight
outline often visits a triangle once; a rectangle can revisit it on different
sides. Reuse across distinct annotation objects would need a shared per-view
runtime cache keyed by the actual source-part identity and triangle range.
The unique-face workload is therefore essential to the adoption decision.
The candidate reserves at most the lesser of segment count and all source-part
triangle counts. A large source part with sparse repeated attachment can reserve
more slots than the actual attached faces. The 64-face fixture avoids this
over-reservation. An implementation needs a measured reserve policy as well as
a reuse threshold; its memory cost cannot be predicted from cache entries alone.

## Results

The serial Release run used a Ryzen 7 5800H and MSVC 19.51.36243.0 with
`/O2 /Ob2 /DNDEBUG`. The table gives median milliseconds for oblique surface
segments over five samples. Each sample is a normalized batched average: time
many rounds, then divide by rounds. The raw file records the rounds count.
Both paths change the camera on every timed build.

| Segments | Current, reused faces | Cached, reused faces | Current, unique faces | Cached, unique faces |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 0.002353 | 0.002645 | 0.002359 | 0.002648 |
| 1,000 | 0.227616 | 0.156251 | 0.226127 | 0.257833 |
| 20,000 | 4.557536 | 3.046688 | 4.589708 | 5.707424 |
| 100,000 | 23.045085 | 15.536600 | 23.052385 | 33.125075 |

At 20,000 segments, reuse cuts this boundary by 33.1%; unique faces increase it
by 24.4%. At 100,000, the changes are 32.6% less and 43.7% more. The 10-segment
reused fixture has ten distinct attached faces, so caching adds overhead with
no slope reuse. The 100,000-segment current oblique samples span 21.86–23.59 ms
for reused faces and 22.11–23.75 ms for unique faces. Full ranges and all flat,
half-bridge, and near-plane cases remain in the raw file. At 100,000 segments,
those production mode medians span 18.59–24.27 ms. This workload cannot sustain
a 16.7 ms CPU frame budget even before the rest of the application runs.

The exact cache asks its allocator for 3,000 bytes with 64 source faces, compared
with 770,040 bytes at 20,000 unique faces and 3,080,184 bytes at 100,000 unique
faces. Each build makes one allocation. Half-bridge cases have fewer entries,
but retain the same reserved allocation. These are requested map bytes during
one build, not process working set or a retained cache. Caching should therefore
be enabled only when attachment reuse can justify its lookup/allocation cost.
The two endpoint distributions do not establish a universal break-even reuse
ratio, especially for larger source parts with over-reservation. Measure real
annotation face distributions before choosing a fixed threshold.

Unchanged-camera cache-hit batched averages are about 45–75 ns at the table's
counts. At 100,000 segments, those checks run only 20 rounds per sample, so clock
quantization and loop overhead materially affect the nanosecond result; samples
range up to 140 ns. Treat this as evidence that a hit avoids the segment loop,
not a precise isolated-call latency.
For fully visible oblique segments, the separate memcpy medians are:

| Segments | Submitted bytes | memcpy, ms |
| ---: | ---: | ---: |
| 10 | 720 | 0.0000203 |
| 1,000 | 72,000 | 0.002391 |
| 20,000 | 1,440,000 | 0.075274 |
| 100,000 | 7,200,000 | 0.330170 |

Thus the persistent-buffer alternative removes a measurable stationary copy
and repeated transient allocation/staging. The numbers measure the copy only;
the allocation, driver, GPU-transfer, and full-frame benefits must be measured
in the application. It does not solve the much larger moving-camera CPU cost.

The exact cached projection matched the entire production vertex array bit for
bit in every timed pair and in the Debug and Release contracts, including bridges,
transformed multi-source segments, near-plane crossings, and 900 nearly edge-on
camera cases. Changing only CPU slope arithmetic from double to float produced
the following maximum absolute NDC depth differences over 180 camera cases each:

| Triangle third-corner y separation | Maximum depth difference |
| ---: | ---: |
| 1 | 2.98e-8 |
| 0.01 | 0.004008 |
| 0.0001 | 0.001407 |
| 0.000001 | 16.775 |
| 0.0000001 | 3,327.041 |

XY output, vertex counts, and finiteness matched in these cases. The depth
differences are material: almost edge-on projected triangles make the plane
solve ill-conditioned, and both paths can produce extreme depth expansion.
This is a CPU arithmetic sensitivity test, not a GPU shader comparison or a
rendered visibility result. It rules out assuming that a direct float port
preserves surface-depth behavior. A shader prototype must define and validate
what happens near those degeneracies before it can merge.

Raw samples and precision cases are in
[simplification-15-results.json](simplification-15-results.json). The shared
[run metadata](simplification-12-14-15-run-metadata.json) records machine,
compiler flags, timestamps, source/generated-copy hashes, and executable hashes.
Reproduce with `experiments/simplification-15/run.ps1` after building the optional
Release probe target; use a fresh output path for a rerun.

## Memory

| Retained or transferred item | Bytes per segment |
| --- | ---: |
| Existing persisted `AnnotationSegment` on x64 | 48 |
| Existing world-line cache | 24 |
| Existing projected vertices, fully visible | 72 |
| Existing projected transfer on each submission | 72 |

At one million fully visible segments, the world and projected caches total
91.6 MiB, in addition to the 45.8 MiB anchors. A shader path can remove the
68.7 MiB CPU projected cache and its per-frame transfer. The world-line cache
still serves CPU edge picking, framing, and dimensions. Removing it needs
separate lazy query work; it must not be counted as an automatic saving.

A fixed GPU anchor format can use 24 bytes of barycentric endpoints, two
triangle IDs, two source IDs, and flags/padding: approximately 48 bytes per
segment. One upload is then 45.8 MiB per million segments. If it simply keeps
one immutable GPU copy and draws generated corners, there is no persistent
72-byte expanded GPU cache. A compute path instead needs projected output plus
a dependency barrier and usually visible-count/indirect-draw handling. Its
transient expansion must be counted. The current adapter allocates staging per
submission; measured peak GPU/CPU memory needs its frames-in-flight retention,
not one isolated array size.

## Smaller alternative

Keep the production projection and upload a persistent GPU vertex buffer only
when `projectionBuilds` changes. A color edit changes the uniform only.
Camera motion still projects and uploads, but a stationary scene stops copying
72 bytes per segment per frame. This change has a smaller numerical surface
because the exact CPU vertices remain the reference. Replacement buffers must
remain alive through submitted frames; GPU writes must use adapter-owned staging
and fences. Measure allocation churn and renderer memory before selecting resize
or growth policy.

A runtime cache can key each persistent buffer by annotation ID, scene owner/
generation, and `projectionBuilds`. It can submit the existing `vs_annotation`
program with a 12-byte position layout. Keep this cache in renderer runtime
storage, outside `UiState` and the persisted annotation geometry. The adapter
already retains vertex-buffer ownership in submitted operations; create/upload
a replacement through its buffer API and release the old handle through the
same API. Validate unchanged-camera upload counts, color-only edits, width/camera
changes, target replacement, scene reset, and deletion while frames remain in
flight. This is a concrete, smaller implementation path with no new scene
property or save-format change.

## GPU prototype and merge requirements

Use the current anchor/source layout to fetch each endpoint from its own source
triangle, then repeat the depth-plane algorithm. Per-source matrices and source
triangle ranges can be shared across segments. Six generated corners in the
vertex shader repeat slope/clipping work; a four-vertex instanced strip reduces
that duplication but changes submission details. A compute shader can do the
projection once, at the cost of output storage and synchronization. Measure both
when the visible-segment count is large.

Floating-point parity is a material risk. CPU slope intermediates are double;
GPU code would normally use float. Current slope guards use `abs(w)>1e-12` and
`abs(determinant)>1e-15`, and contraction can change a nearly edge-on result.
Render triangles use float positions, but that does not prove double slope
arithmetic is redundant. Compare captured vertices with tolerance and compare
rendered coverage/depth; bit equality alone is not a useful universal GPU gate.

Require ordinary/bridge/multiple-source segments; flat, steep, edge-on, collapsed,
near/far crossing triangles; perspective and orthographic views; very large
coordinate origins and small near planes; reversed depth; hidden/missing/reloaded
targets; color/width edits; cancellation; save/load and undo; retained-frame
lifetime; sampled drag preview without surface depth; and high-DPI/export pixel
scale. Use occluders to expose an incorrect depth bias. Compare both Vulkan and
Metal. Measure longest CPU stall, CPU submission time, GPU timestamps, transfers,
and retained/peak storage under stationary views and continuous orbit. A CPU
cost that justifies implementation does not authorize merging an unvalidated
GPU shader.
