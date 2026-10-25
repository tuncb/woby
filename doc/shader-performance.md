# Shader performance audit

The retained change specializes opaque point rasterization for one or four
samples. In the application, full-detail GPU time fell by 11% to 16% on the
100-million-point Semantic3D input. BearTrap with surfaces, edges, and 8-pixel
markers improved by 10%. BearTrap at 4 pixels was effectively unchanged.
Exported pixels were identical. Adaptive and cached-frame results varied
between campaigns. No stable speed claim is made for those paths.

This audit uses the application at `f83fb21` as its reference. Measurements
ran on 24 and 25 October 2026. The reference already has fused triangle edges,
compact compute points, atomic-load rejection, complete-tile certificates, and
surface-depth seeds. Those improvements are not new results from this audit.

## Retained change

`opaque_points.slang` selects the sample count once per point task. Slang value
generics then provide fixed sample positions, loop bounds, and winner-buffer
strides. The adaptor already accepts only one or four samples. There is no
new pipeline key, point format, setting, or quality reduction. Both the 64-bit
atomic path and the portable 32-bit path use the same specialization.
See Slang's [value generic parameters](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/06-interfaces-generics.html).

Only `cs_opaque_raster`, `cs_opaque_depth`, and `cs_opaque_ids` change in the
production build. The other 41 shader binaries are unchanged. All 44 production
binaries match the measured `sample-generic` candidate byte for byte.

### Paired rendering measurements

These rows use the retained source. GPU time is the median of round medians.
The reduction pairs each camera frame before it takes the median, so it can
differ from the ratio of the two displayed times. Each row passed exact color,
depth, and picking-ID checks at both validation angles.

| Input and view | Samples | Marker size | Reference, ms | Retained, ms | Paired reduction |
|---|---:|---:|---:|---:|---:|
| Semantic3D, fitted | 4 | 1 px | 7.950 | 7.072 | 12.5% |
| Semantic3D, fitted | 4 | 4 px | 26.019 | 23.811 | 8.3% |
| Semantic3D, fitted | 4 | 8 px | 74.563 | 61.914 | 16.5% |
| Semantic3D, close | 4 | 4 px | 27.602 | 23.196 | 15.9% |
| Semantic3D, close | 4 | 8 px | 82.274 | 65.005 | 21.0% |
| Semantic3D, fitted at 1920 by 1080 | 1 | 1 px | 5.190 | 4.513 | 12.0% |
| Semantic3D, fitted at 1920 by 1080 | 1 | 4 px | 14.200 | 12.238 | 12.6% |
| Semantic3D, fitted at 1920 by 1080 | 1 | 8 px | 25.393 | 20.966 | 17.5% |
| BearTrap, surfaces, edges, and points | 4 | 8 px | 35.384 | 33.050 | 6.8% |
| BearTrap, points only | 4 | 8 px | 28.651 | 22.567 | 20.8% |

Sources: `semantic100m-generic`, `semantic100m-controls`,
`semantic100m-single1080`, and `beartrap-controls` in the
[raw summary](benchmarks/shader-audit-20261024/summary.csv).
The earlier `beartrap-generic` run found only 2.3% for combined rendering and
6.5% for points alone at 8 pixels. This variation limits any general BearTrap
speed claim. The application check below measures its actual combined path.

Three source forms of the sample specialization produce identical instructions
after `spirv-opt --strip-debug --compact-ids`. Their unmodified shader binaries
can still produce different timings. The additional normalized controls retain
the Semantic3D gain: 13.6% and 18.1% in the close view. Normalizing the baseline
alone changes those timings by only 2.1% and 3.0%. The controls support the sample
specialization, but do not establish the driver-level cause of the remaining
variation. Normalization is not added to the production build.

### Application measurements

The existing application runner uses two rounds, 24 full-detail camera commands,
and 12 adaptive-navigation commands per case. These are serial runs, not paired
per-frame measurements. The hidden window uses 20 Hz pacing. The table reports
whole-frame GPU time for full-detail rendering, not FPS or input latency.

| Input, 4x MSAA | Marker size | Reference, ms | Retained, ms | Reduction |
|---|---:|---:|---:|---:|
| Semantic3D, points only | 4 px | 23.658 | 21.033 | 11.1% |
| Semantic3D, points only | 8 px | 67.998 | 57.798 | 15.0% |
| BearTrap, surfaces, edges, and points | 4 px | 24.295 | 24.200 | 0.4% |
| BearTrap, surfaces, edges, and points | 8 px | 38.428 | 34.458 | 10.3% |

All nine reference/retained PNG pairs, including the longer check below, have
identical pixels. The camera, model
hash, drawable size, MSAA setting, and source counts match. No resource guard
fired. In this short campaign, cached GPU time stays near 0.25 to 0.29 ms for
Semantic3D and 16.6 to 16.7 ms for BearTrap, whose mesh still draws each frame.

In the short adaptive run, Semantic3D submits the same 7,999,905 points in both
versions. At 4 pixels, its GPU median changes from 2.775 to 3.073 ms. At 8 pixels,
it changes from 6.807 to 6.616 ms. BearTrap navigation changes by less than 4%.
The full-detail gain must not be applied to cached or adaptive frames.

A further Semantic3D run uses three rounds and 96 navigation commands per case,
with the retained application measured first. It confirms the full-detail
gain: 23.944 to 20.325 ms at 4 pixels and 67.273 to 56.579 ms at 8 pixels, or
15.1% and 15.9%. Navigation changes from 8.127 to 7.737 ms at 4 pixels and
7.824 to 7.064 ms at 8 pixels. The short-run 4-pixel slowdown did not repeat.
However, cached times also vary greatly in this longer campaign despite zero
point-raster work. Thus, the sparse-workload results do not isolate the shader
change from other run conditions. Clocks were not fixed, and their contribution
was not measured. Keep the navigation and cache results as controls, not as a
claimed improvement.
See [application measurements and pixel checks](benchmarks/shader-audit-20261024/application-summary.json).

## Scope

The application builds 44 shader entries. All entries come from
`shaders/native/woby.slang` and `shaders/native/opaque_points.slang`.

| Area | Entries reviewed | Checks and candidates |
|---|---|---|
| Mesh vertices | `vs_mesh`, `vs_color`, `vs_annotation`, `vs_comparison`, `vs_mesh_edges`, `vs_triangle_lines` | Vertex fetches, UV loads, normal transforms, separate UV storage, indexed reuse, pulled edges. |
| Mesh fragments | `fs_mesh`, `fs_color`, `fs_comparison`, `fs_mesh_edges`, `fs_mesh_edges_pulled`, `fs_marker_mesh`, `fs_marker_mesh_edges`, `fs_marker_mesh_edges_pulled`, `fs_marker_line`, `fs_marker_comparison` | Lighting, edge derivatives, squared distances, early depth tests, ID writes, discard rules. |
| Sprites | `vs_point_sprite`, `vs_line_sprite`, `vs_marker_point`, `fs_point_sprite`, `fs_marker_point` | Clip tests, repeated transforms, circle tests, sample coverage, source colors and IDs. |
| Transparency | `fs_transparent_mesh`, `fs_transparent_edges`, `fs_transparent_edges_pulled`, `fs_transparency_resolve_single`, `fs_transparency_resolve_msaa` | Cubic weights, half-float accumulation, depth reconstruction, sample reads, resolve cost. |
| Picking and composition | `vs_marker_screen`, `fs_marker_composite`, `vs_marker_highlight`, `fs_marker_highlight_single`, `fs_marker_highlight_msaa`, `cs_marker_lookup_single`, `cs_marker_lookup_msaa` | Search bounds, distance pruning, tie order, bounded highlight rectangle, packed IDs. |
| UI | `vs_imgui`, `fs_imgui` | Vertex decoding, texture sampling, blending, draw clipping. |
| Opaque points | `cs_opaque_clear`, `cs_opaque_raster`, `cs_opaque_batch_clear`, `cs_opaque_depth`, `cs_opaque_ids`, `cs_opaque_merge`, `vs_opaque_resolve`, `fs_opaque_resolve`, `fs_opaque_color` | Workgroup width, atomic updates, tile certificates, sample loops, bounds rejection, clear work, group lookup, portable fallback. |

The six additional Slang experiment files and six older bgfx `.sc` files are
research or test sources. They are not additional application shader stages.
Some retained experiments use an earlier root ABI. This audit does not rewrite
those historical controls or count their earlier gains as current improvements.

## Measurement method

The GPU is an NVIDIA RTX 3070 Laptop with 8 GiB of VRAM, driver 616.92.
The machine has a Ryzen 7 5800H and 64 GiB of RAM. The local Release build uses
`vs2026-vcpkg`, Slang 2026.18.3, and NoGraphicsAPI Vulkan.

The optional point-pipeline probe now compares complete shader directories.
Each camera frame runs the reference, an unchanged repeat control, and the
candidates. Their order rotates each frame and round. Most large-model screens
use 12 or 16 measured frames per round and three rounds. Final candidate checks
use 24 frames and three or four rounds. The retained JSON gives
the exact settings. There are four warm frames per round.

The usual target is 1280 by 687 pixels at 4x MSAA. The close view uses 0.35 times
the fitted camera distance. GPU timestamps include all render passes, clears,
barriers, and resolves. They exclude capture and readback. Each frame completes
before the next starts. These are GPU rendering times, not application FPS.

Before each timed workload, two camera angles compare every final color, depth,
and original picking-ID sample. Color and IDs use exact packed-byte equality.
Depth uses exact D32 bit equality. Both sides use the same surface-depth seed
rules. Point tests retain all source points. They do not use adaptive detail.

The summary pairs individual camera-frame times before calculating ratios.
It reports the median of round medians and their range. An unchanged repeat
control measures noise in each campaign. GPU clocks and temperature are not
fixed. A small difference, or a gain that disappears on another run, is not
enough reason to change the application.
No hardware performance-counter capture was used. Memory traffic and occupancy
are reasons to test candidates, not confirmed diagnoses from these timings.

Campaigns run separately from builds, tests, and other monitored Woby work.
The runner stops above 38 GiB of private memory, below 6 GiB of available memory,
or after 20 minutes. It records model, source, executable, and shader hashes.
An incomplete run cannot enter the summary.

## Large test inputs

| Input | Source size or geometry | Purpose |
|---|---|---|
| BearTrap ground model | 7.11 GB; 75,771,986 triangles; 38,414,391 markers | Dense terrain, combined surfaces and points, fitted and close views. |
| Bennu OLA very high | 17,866,836 triangles; 8,933,524 markers | Dense indexed surfaces, edges, UVs, transparency, sprites. |
| Powerplant | About 12.76 million triangles; 21 groups | Multiple draw ranges and occlusion. |
| San Miguel | About 9.98 million triangles; 2,203 groups | Many draw ranges and mixed surface coverage. |
| BusGameMap | About 1.05 million triangles; 65 groups | Smaller mesh and different vertex reuse. |
| Semantic3D station 8 | 100,000,000 source points | Large point-only input with no surface-depth seed. |

These are the same local datasets used in the earlier rendering investigations.
The evidence records their full paths and SHA-256 hashes.

## Ideas tested

The audit built 40 candidate or control directories. Timing campaigns include
39 of them, plus the reference and its unchanged repeat. `sample-shared` was
checked by compilation and normalized binary comparison only. The 32 complete
campaigns contain 35,986 measured GPU frames and 2,886 image-sample comparisons,
covering 4,645,062,480 sample pairs. Each pair checks color, depth, and ID.
These counts include fixture runs and reference controls, not just large models.

| Candidate | Reason to test | Decision |
|---|---|---|
| 32, 64, 256, 512, and 1,024 threads per point group | Change occupancy and latency hiding. | 256 threads helped an early BearTrap screen but did not repeat in the combined path. It slowed the 100-million-point input at 1 and 4 pixels. No unconditional change. |
| Slang `-O2` and `-O3`, with several workgroup widths | Reduce generated code and repeated loads. | `-O2` and `-O3` produced identical SPIR-V for all 44 entries. Smaller code did not give a consistent GPU gain. |
| Direct atomics and subgroup aggregation | Reduce atomic-read or update traffic. | Removing the winner guard loses useful rejection. The tested subgroup aggregation was much slower. |
| Direct tile-certificate update and 32-bit depth certificates | Reduce certificate cost. | Close BearTrap results were small and varied by marker size. The depth-only certificate must retain equal-depth candidates. |
| Untiled loop, sample mask, and complete-pixel test | Remove nested-loop or repeated circle-test work. | No consistent gain. The complete-pixel test adds work for partial pixels. |
| Task AABB rejection | Skip offscreen blocks before loading point positions. | Some close Semantic3D cases gained 8% to 9%, but other views and BearTrap did not show a consistent gain. No production buffer or ABI change. |
| Clear active tiles only; bypass one-group search | Reduce fixed per-frame work. | The tested end-to-end differences were within noise. |
| Conditional UV reads and separate vertex streams | Reduce vertex data traffic. | No consistent gain across Bennu, BearTrap, Powerplant, San Miguel, and BusGameMap. Occasional combined-path gains did not repeat across marker sizes. Keep the current layout. |
| Separate streams with conditional UV reads together | Remove unused UV traffic from the separate stream. | No gain on BearTrap. Powerplant's solid pass showed 6.1%, while its unchanged repeat showed 8.0%; other passes did not improve. Reject this combination too. |
| Skip identity normal transform | Remove vertex arithmetic in the common identity case. | No reliable gain across the four layout campaigns. The prototype also needs a guarded choice for non-identity transforms. Keep the current transform. |
| Forced early depth tests | Avoid shading hidden opaque fragments. | No reliable gain in the opaque mesh campaigns. Hidden-line interiors and zero-alpha draws also need their discard behavior. Keep the existing depth rules. |
| Squared edge distances and circle tests | Reduce square-root work. | No reliable gain on Bennu. Keep the existing expressions. |
| Explicit cubic transparency weights | Replace general power operations. | No reliable gain on Bennu. Keep the existing weight calculation. |
| Trivial line clip acceptance | Avoid six clipping planes for fully visible lines. | No reliable gain in the synthetic line-sprite workload. |
| Fixed sample counts | Remove runtime sample work from inner loops. | Retain `sample-generic`. It gives the broadest useful measured improvement with one shared source body. |
| Also specialize the query flag | Remove the task's query branch. | Weaker results overall than sample-count specialization alone. Do not add more code variants. |
| One sample per invocation | Change work distribution across lanes. | Much slower on the 100-million-point input: about 74 ms instead of 26 ms at 4 pixels, and 160 ms instead of 75 ms at 8 pixels. Reject. |
| Lossless float-bit packing | Reduce point reads from 16 to 12 bytes where task ranges fit. | Two 16-bit offsets packed no points on BearTrap or Semantic3D. The 21/21/22-bit format packed 32.3 million Semantic3D points, but was no faster and often slower. Reject; retain full float positions. |

The picking lookup reads at most a 7 by 7 pixel neighborhood, with a radius
test and one or four samples. A proposed best-distance prune preserves ties,
but this small dispatch does not process the large model. The highlight draw
already uses a bounded rectangle. ImGui and the composite shader each have a
simple texture sample. There is no evidence that these stages limit the large
geometry workloads. Their existing application GPU tests remain necessary.

## Correctness limits

The probe's comparison mode uses a proxy scalar field. It does not replace
real detector-result tests. Its line-sprite mode forms segments from triangle
indices. It does not represent every authored line model. Model transforms in
the timing probe are identity; the camera changes yaw. A pass in this probe
does not make an identity-only normal optimization safe for other transforms.

The split-stream and bounds candidates use spare probe root fields. They are
research prototypes. Production use would need proper buffer ownership, ABI
fields, worker preparation, invalidation, and loading tests.

The first multi-group timing attempt exposed two errors in the new probe:
unaligned per-group roots and a transparency resolve that overwrote a surface
root before submission. Both are fixed in the probe. The application uses its
existing aligned root allocator. The interrupted Powerplant runs are excluded.
The regression fixture now has four groups and checks both sample counts.

Do not remove the resolve shader's address clamps. Fragment helper lanes can
execute outside a non-aligned viewport. Also do not force early depth writes
for shaders whose discard determines whether a fragment can write depth.
See the Vulkan [depth guide](https://docs.vulkan.org/guide/latest/depth.html).

Vulkan 1.4 requires support for workgroups of at least 256 invocations, but does not
make the tested 512- or 1,024-thread settings portable defaults. The application
also has a Metal path. See the Vulkan [device limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).
An NVIDIA timing result alone does not establish a Metal performance gain.

## Regression checks

The `vs2026-vcpkg` Release build completed without compiler warnings. The full
CTest run passed 133 of 134 entries. `woby_headless_camera` failed because a
screenshot CLI command returned empty JSON output. The unchanged test then
passed three consecutive reruns. No shader or test workaround was added for
that failure. All 134 entries have passing results; the initial failure and
the rerun logs are retained.

The new GPU regression changes the sample count through 1, 4, 1, and 4 while
it keeps the compute programs and mesh. It checks original IDs and source
colors against the CPU reference for both atomic backends. It passed against
the original shaders before the production change and against the retained
shaders afterwards. Existing tests cover dense contention, equal-depth ties,
clipped query rectangles, transformed points, seeded surfaces, cached frames,
and the quad fallback.

The audit's four-group fixture also checks every final sample at both sample
counts, including nonzero viewport origins, clipped triangles, transparency,
and pulled edges. Reporting tests reject missing or empty timing data, invalid
times, unpaired frames, missing comparisons, and zero-sample comparisons.

All 44 production SPIR-V entries pass `spirv-val`. All 44 also translate to
Metal source with Slang warnings treated as errors. An Apple native compiler
and Metal GPU were not available, so this is not a Metal build or speed test.
The point GPU regressions and audit fixtures pass with Vulkan core and
synchronization validation enabled, with no validation warnings or errors.
The [verification manifest](benchmarks/shader-audit-20261024/verification.json)
records shader hashes and translation commands.

## Reproduction and evidence

See the [probe instructions](../experiments/point-pipeline/README.md#shader-audit).
The [evidence directory](benchmarks/shader-audit-20261024/) retains configurations,
raw timings, validation counts, source hashes, candidate manifests, and results.
Shader binaries and large input models remain outside version control.

Earlier work remains relevant:

- [Triangle edge integration](shader-edge-integration.md)
- [Point navigation](point-navigation-performance.md)
- [Surface-depth seeds and point pipeline experiments](point-pipeline-performance.md)
- [Large-model rendering](large-model-rendering-performance.md)
