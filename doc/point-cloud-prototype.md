# Opaque point rendering prototype — 2026-10-04

The 100M-point experiment supports **spatial detail selection during navigation,
followed by persistent visibility refinement when still**. Changing the primitive
to compute alone is insufficient: brute-force compute is slower than the quad
control for 4- and 8-pixel points at 4× MSAA. Bounding the submitted work and
reusing completed visibility are the decisive changes.

This is an opt-in, headless prototype. The production vertex renderer is unchanged.
The source mesh, original point identities, and precise positions are retained.
All points and surfaces in these measurements are opaque.

## 100M-point results

Semantic3D `semantic3d_sg27_station8_100000000_xyz_points.obj`, RTX 3070 Laptop
8 GiB, Ryzen 7 5800H, 64 GiB RAM, NVIDIA driver 616.92, Windows Vulkan.
Fixed **1280×720, 4× MSAA**, with IDs enabled in both implementations.

| Point diameter | Original full-data quads, GPU ms | Full-data compute, GPU ms | Adaptive navigation, GPU ms | Navigation total median / p95 ms | Retained sample coverage |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 px | 173.17 | 58.46 | 0.52 | 3.49 / 5.26 | 99.24% |
| 4 px | 470.08 | 541.42 | 3.84 | 6.85 / 8.67 | 99.85% |
| 8 px | 1179.13 | 1587.88 | 7.96 | 10.85 / 12.15 | 99.92% |

Navigation total includes CPU hierarchy selection, command submission, and GPU
completion wait. It excludes application UI and presentation, so these numbers
are not measured interactive Woby FPS. The full-data columns use the fixed fitted
camera; navigation uses a 90-frame orbit. Detail differs deliberately, and no
pixel-equivalence speedup ratio is claimed between those workloads.

At **0.25× camera distance**, 4-pixel navigation took **7.31 ms median / 9.16 ms
p95**, retaining **99.26%** of the full reference's covered samples. Maximum
observed navigation frame time was 9.99 ms in that closer view and 12.57 ms in
the fitted 8-pixel case. The fitted cloud occupies a relatively small region of
the viewport; the closer view tests much greater screen coverage.

Coverage is measured against full-data **compute** rendering at the final orbit
camera, taking the lowest coverage across the three rounds. It counts samples
with a visible point, not identical point IDs or depth. There were no additional
covered samples outside the full reference. Missing coverage remains a visual
approximation, and these views do not establish a guarantee for arbitrary thin
features, abrupt camera changes, or every viewpoint.

Close view during navigation:

![Navigation at 4 pixels](benchmarks/point-cloud-20261004/cloud-close-navigation.png)

The same camera after full refinement:

![Full-data reference](benchmarks/point-cloud-20261004/cloud-close-full.png)

## Refinement and exact source queries

The coarse view remains visible while bounded batches accumulate original points.
All **18 measured refinement sequences** ended with byte-identical RGBA and
identical IDs at every MSAA sample compared with a fresh full-data compute draw.
The cloud sequences each visited all 100,000,000 original points. No visibility
readback or CPU oracle is needed to choose a rendering winner.

| Cloud view | Refinement frames | Total unpaced computation, median | Frame-count time at 60 Hz |
| --- | ---: | ---: | ---: |
| Fit, 1 px | 50 | 74 ms | 0.83 s |
| Fit, 4 px | 81 | 630 ms | 1.35 s |
| Fit, 8 px | 233–238 | 1928 ms | 3.88–3.97 s |
| Close, 4 px | 55 | 374 ms | 0.92 s |

The last column is an estimate from the frame count, not a presentation
measurement. A production scheduler could spend more of an idle frame budget
on refinement. The 8 ms controller is feedback rather than a hard deadline:
the largest observed refinement frame was 15.31 ms. Once fully refined, cached
visibility resolved in about **0.09 ms GPU**, or **0.3–0.4 ms total** headlessly.

Precise queries traverse original hierarchy leaves rather than proxy points.
All **128 covered-sample queries** in the measured cloud runs matched the full
GPU reference ID, including 4× sample locations. At 4 pixels, fitted-view query
timing was **0.63 ms median / 1.44 ms p95**; the close view was **1.34 / 3.68 ms**.
These queries use the original render positions and return IDs mapped back to
the retained mesh's precise positions. Measurement tools and source-data
ownership have not been rewritten as part of this experiment.

## Opaque mesh vertices

BearTrap has 38,414,391 submitted markers and 75,771,986 triangles. With 4-pixel
vertices and opaque surfaces, the original control was about **99.28 ms GPU**.
Full-data compute was also about **98 ms GPU**. The surface pass alone costs
approximately 15 ms, so a universal 8 ms GPU-frame target is inappropriate.

| Navigation policy | GPU median | Total median / p95 | Minimum marker coverage |
| --- | ---: | ---: | ---: |
| 8 ms target | 15.56 ms | 17.22 / 17.88 ms | 73.21% |
| 20 ms target, up to 2M points | 18.44 ms | 23.89 / 27.03 ms | 98.66% |

The first policy leaves only the controller's minimum raster allowance and
visibly undersamples the vertices. The second is substantially more useful, but
still spends about 5 ms on CPU selection/submission. Its stationary view reached
the exact result in 20 refinement frames, about 93 ms of unpaced computation.
This is evidence for a separate mesh-overlay quality policy and cheaper
hierarchy traversal, not a claim that mesh rendering now sustains 60 Hz.

![BearTrap navigation with the larger budget](benchmarks/point-cloud-20261004/beartrap-navigation.png)

The first BearTrap control round transitioned from approximately 143 to 99 ms
mid-round; its conventional median was 121 ms. The other two round medians were
99.26–99.28 ms. A compute round also ran slower. The cause was not established;
all samples are retained, and the comparison uses the lower, repeatable control
cost rather than treating the slow round as a larger improvement.

## Representation and memory

- Each render point contains an unquantized 12-byte position and a 4-byte original
  marker ID. Morton keys only reorder data; they do not alter positions.
- A spatial binary hierarchy partitions that order into leaves of at most 4096
  points. Nodes contain stratified original-point samples plus actual extrema.
  The cloud has 65,535 nodes and 8,752,756 proxy points.
- Navigation selects a hierarchy frontier using projected bounds and sampling
  error. It starts with a 500k-point budget, adjusts from GPU timing, and submits
  at most 2M points. Metadata traversal replaces per-frame CPU scans of 100M points.
- Compact data is uploaded in independent allocations of at most 1,048,576 points
  (16 MiB), with bounded staging. All allocations are resident in this prototype.
- A 64-bit atomic maximum chooses the reversed depth and original ID together
  for each sample. Equal-depth ties use the original ID, independent of storage
  or submission order. Complete circular footprints compete with opaque surface
  depth; visible circle fringes are retained even when their centers are hidden.
- Shading resolves winning IDs afterward. Stationary refinement preserves this
  buffer. Camera, point size, surface mode, group visibility, transform, and color
  changes reset it. Immutable geometry and fixed viewport are prototype contracts.

For the cloud, retained GPU geometry fell from **3.600 GB to 1.740 GB** including
proxies, a **51.7% reduction**. The visibility buffer adds **29.49 MB** at the tested
resolution and sample count. These are decimal logical buffer sizes, not total
device-memory measurements; texture heaps, upload staging, readback, roots, and
task metadata are additional allocations.

The mesh adapter still retains the original surface vertex/index buffers.
Consequently, BearTrap geometry **increased from 2.292 GB to 2.823 GB** when adding
the compact marker hierarchy. Production integration should share point positions
with the surface representation and preserve instancing rather than retain both
forms. The current point records can also duplicate shared vertices across groups.

The CPU adapter keeps the existing Mesh/source copies and adds about 2.145 GB of
compact cloud data and mappings. Hierarchy construction took **7.9–9.2 seconds**
for the cloud, after existing OBJ loading, and **5.1–5.5 seconds** for BearTrap.
This is offline setup, not an asynchronous loading implementation. Across the six
final campaigns, peak sampled process-private memory was **12.17 GiB** and minimum
available system memory was **35.71 GiB**. No overlap or resource guard fired.

## Implementation boundaries and next work

The implementation is in [point_cloud.cpp](../experiments/mesh-overlays/point_cloud.cpp),
[point_renderer.cpp](../experiments/mesh-overlays/point_renderer.cpp), and
[point.slang](../experiments/mesh-overlays/point.slang). It builds on the compute
visibility-buffer approach described in
[Software Rasterization of 2 Billion Points in Real Time](https://www.cg.tuwien.ac.at/research/publications/2022/SCHUETZ-2022-PCC/).
That paper's headline implementation concerns one-pixel points; these measured
full circles and MSAA workloads have different costs.

The prototype directly visits footprint samples. It does not yet implement tile
lists, quantized/chunk-relative coordinates, per-point colors, disk paging,
incremental hierarchy construction, asynchronous UI integration, or a Metal/
32-bit-atomic fallback. The old importer/preparation capacity limits still apply,
even though the new point GPU allocations are chunked. It assumes Vulkan standard
1×/4× sample locations, verified by the GPU oracle tests on this device.

The compute circle test runs at each MSAA sample. The original quad shader
discards the circle at pixel frequency, so edge coverage differs. This prototype
is not a pixel-identical replacement for the old MSAA path. Selected diagnostic
markers and production measurement/picking interactions still need integration.

The next production work should keep the hierarchy and original identities
independent of the raster backend:

1. Coordinate point/source ownership and chunked import with issues
   [112](https://github.com/tuncb/woby/issues/112) and
   [113](https://github.com/tuncb/woby/issues/113), removing redundant Mesh copies
   and making loading, residency, cancellation, and uploads bounded.
2. Move or amortize hierarchy selection off the UI thread; add stable selection,
   camera-motion tests, thin-feature quality checks, and explicit marker quality
   floors when surfaces consume the frame budget.
3. Compare tiled compute and hardware rendering for large/sparse footprints.
   The direct atomic implementation is an exact full-footprint reference;
   its brute-force 4× MSAA cost is not the final rasterization target.
4. Integrate precise queries and pinned selected markers with the authoritative
   source data, then validate frame pacing and interaction in the actual viewer.

## Reproduction and evidence

[Build and CLI instructions](../experiments/mesh-overlays/README.md#compact-opaque-point-renderer)
describe the separate `woby_point_prototype` target. Six serial final campaigns
used one executable SHA-256:
`d519dcfe5876f0d5600244026e9e863c9cac9de53b86ae007ddb4b884842b222`.

[Summary](benchmarks/point-cloud-20261004/summary.json) links the original raw
directories. The adjacent JSON files retain every GPU/CPU/wall sample, coverage,
camera settings, point counts, refinement comparisons, guards, and source/input/
SPIR-V hashes. Each final case has three rounds. Full-data cases have three warmup
frames and 12 measured frames; cached cases have 12 measured frames after
refinement, and each orbit has 90 frames.
Point-size order rotates between rounds. Captures, source queries, and the full
reference used for comparisons are outside measured navigation/refinement frames.
Navigation traces follow the full-data workload; cold-start navigation and
resuming from a prolonged idle period were not measured.
The control's upload timer excludes device initialization, whereas compute setup
includes it; those setup fields should not be treated as identical boundaries.

Tables use medians of the three conventional per-round medians recomputed from
raw samples. P95 is the highest round's stored higher-order-statistic percentile.
Coverage is the lowest of the three final-camera measurements. Exploratory 1×
runs and looser sampling pilots are excluded from these tables.

Behavioral tests cover ID/position preservation, hierarchy budgets, tiny budgets,
full-source refinement partitions, source picking, complete circle fringes,
clipping, 1×/4× sample coverage against an independent CPU oracle, chunk boundaries,
group transforms, stable ties, opaque surfaces, order-independent accumulation,
cache invalidation, and the workload guard. The GPU tests also run with Vulkan
synchronization validation; shaders compile with warnings treated as errors and
pass SPIR-V validation.

Validation completed with warning-free full Debug and prototype Release builds,
**all 918 Debug CTest entries passing** (300.56 seconds), the three focused
Release checks passing, and the final Debug GPU checks passing with Vulkan
synchronization validation enabled. `git diff --check` passed.
