# Point drawing during camera movement

Measured on 23 October 2026. Baseline: `ef2112afd50410b4189484e120ebb27c29fde956`.

The [follow-up pipeline experiments](point-pipeline-performance.md) compare early
surface depth, precise barriers, and a combined renderer against the optimized
renderer described here.

Keep **Adaptive points while navigating** for now. The new renderer makes full
detail much faster and preserves the original sample results. However, full
detail still exceeds a 33.3 ms GPU frame budget in several tested views. Removing
the option would remove a useful way to keep the camera responsive.

After review, the option is **off by default**. A saved scene or saved view can
explicitly enable it. A scene or view without the setting also defaults to off.
The measurements below explicitly select full or adaptive detail for each phase.

The implemented alternative reduces the loss of detail during motion. The
adaptive ceiling increases from 2 million to 8 million points. The initial
budget stays at 2 million, and the existing GPU timing controller still targets
8 ms for the point pass. A scene that fits the current budget now uses all its
points, even if its groups have different sizes.

## Model and equipment

The large mesh in the earlier reports is
`D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj`:

- 7,108,991,827 bytes: 7.11 GB, or 6.62 GiB.
- 38,414,391 vertex markers and 75,771,986 triangles.
- One group.
- SHA-256: `7abcb6005617270f3c799292991b858a16e0cddf39b9655eb3f1d691b2a45329`.

The machine has a Ryzen 7 5800H, 64 GiB of RAM, and an RTX 3070 Laptop GPU with
8 GiB of VRAM. Tests used Windows 11, NVIDIA driver 616.92, MSVC 19.51.36243.0,
and Slang 2026.18.3. The renderer is **NoGraphicsAPI Vulkan**. Earlier reports
about bgfx or D3D11 describe a different renderer and are not the baseline here.

The other inputs were the 100-million-point Semantic3D station scan, the
8,933,524-vertex Bennu mesh, and the 547,469-vertex BusGameMap mesh. Their paths
and hashes are in the measurement records.

## Full-detail results

Times are GPU milliseconds; lower is better. Each value is the median of three
round medians. The ranges contain those three medians. Mesh measurements include
solid surfaces, triangle edges, and opaque vertex markers. Adaptive detail is
**off** in this table.

| Model and view | Circle diameter | Original | Optimized | Speedup | Original range | Optimized range |
|---|---:|---:|---:|---:|---:|---:|
| BearTrap, fitted, confirmation pair | 4 px | 122.64 | 41.37 | 2.96x | 121.33–124.87 | 33.34–41.94 |
| BearTrap, fitted, confirmation pair | 8 px | 405.00 | 63.38 | 6.39x | 403.32–407.00 | 51.56–69.00 |
| BearTrap, close | 4 px | 92.54 | 43.61 | 2.12x | 91.87–92.76 | 37.89–44.24 |
| BearTrap, close | 8 px | 320.93 | 73.29 | 4.38x | 318.01–346.44 | 66.57–77.12 |
| Semantic3D, 100M points | 4 px | 572.60 | 30.65 | 18.68x | 566.91–689.33 | 30.60–31.11 |
| Semantic3D, 100M points | 8 px | 1,679.73 | 75.23 | 22.33x | 1,470.83–1,682.17 | 74.37–75.46 |
| Bennu | 4 px | 30.24 | 10.94 | 2.77x | 26.16–30.68 | 10.86–11.31 |
| Bennu | 8 px | 84.48 | 16.34 | 5.17x | 84.02–97.84 | 16.30–17.45 |

The BearTrap confirmation pair was run after the other campaigns. It is the
conservative headline result. Earlier runs with the **same optimized shader**
gave 25.85 ms at 4 px and 37.27 ms at 8 px. The corresponding original times
were 100.27 and 348.27 ms. The later 8M-ceiling campaign gave 33.52 and 54.92 ms.
The ceiling cannot change full-detail task selection when adaptive detail is off.

The variation also affects the unchanged mesh passes: the cached BearTrap frame
rose from about 16.5 ms to about 25 ms across campaigns. GPU clocks and thermals
were not fixed. The confirmation telemetry recorded 60–79 degrees C and varying
clocks, including idle periods. This establishes variation; it does not isolate
its cause. Do not treat the best run as a guaranteed frame time or attribute all
differences between campaigns to the code.

The full-detail benefit is large in every campaign. The remaining variation is
also large enough to prevent a general smooth-navigation claim.

## More detail during motion

These are adaptive measurements from the confirmation pair. Submitted counts
come from changed-camera frames. They count submitted points, not distinct
visible pixels. The controller can select a smaller cut when point drawing is
expensive.

| BearTrap | Original points | New points | Original GPU time | New GPU time | New worst round p95 |
|---|---:|---:|---:|---:|---:|
| 4 px | 1,999,991 | 7,999,895 | 26.11 ms | 28.81 ms | 30.84 ms |
| 8 px | 444,035 | 5,169,339 | 27.23 ms | 31.83 ms | 36.33 ms |

At 4 px, the new path retains four times as many submitted points. At 8 px, it
retains about twelve times as many in this pair. The point pass itself takes
4.22 and 7.32 ms. Much of the total frame is the mesh. More detail has a cost:
the adaptive total is slightly slower here, and the 8 px p95 exceeds 33.3 ms.
The 8 ms point target is feedback, not a hard bound on frame latency.

In the earlier 8M campaign, BearTrap submitted 8.0M points at 4 px and 6.46M at
8 px. In the close view it submitted 8.0M and 3.31M. The 100M scan submitted
about 8.0M at both sizes, with total medians of 2.82 and 6.10 ms. This is useful
headroom for detail, but it does not mean that all 100M points can be drawn at
the same cost.

The small-scene correction is separate from the higher ceiling. The old equal
division of the budget reduced a large group even if other groups used almost
none of their shares. BusGameMap exposed this: it has 65 groups but only 547,469
markers in total. The runtime now selects the full source whenever the total
fits the current budget. It also omits the unnecessary asynchronous reduced-cut
job in that case. No new user setting is needed.

The final BusGameMap run submits all 547,469 source markers on changed-camera
frames, compared with 99,097 in the baseline. Adaptive GPU medians are 1.57 ms
at 4 px and 3.91 ms at 8 px. The largest round p95 values are 3.60 and 5.39 ms.
Full-detail medians are 1.87 and 3.90 ms, compared with 2.80 and 6.05 ms before
the renderer changes. The option now has no visible reduction on this model
while its full source fits the timing budget.

## What changed in the renderer

The original compute shader performed a global 64-bit atomic maximum for each
covered circle sample. In a dense model, many points repeatedly update the same
samples even though they cannot become visible. Larger circles multiply this
work. This is a raster and atomic contention problem, not primarily a file-load
or CPU submission problem.

The new path has three parts:

1. Before an atomic maximum, read the current winner with a **relaxed atomic
   load**. Skip the update if the candidate cannot win. An ordinary unsynchronized
   load would be incorrect here.
2. Keep a depth/ID bound for each screen tile. A point may publish a bound only
   if its circle covers every sample of that tile and its query covers the
   complete tile. A later point with a lower or equal key can skip that tile.
3. Unroll the four-sample loop. Use power-of-two tile sizes and shifts, so each
   point does not need runtime integer division.

The tile test is exact. The key contains reversed depth and the original vertex
ID. Keys only increase. A point that publishes a bound must still write all its
covered samples before the dispatch ends. Even if another invocation reads that
bound before those writes, the published point will supply an equal or greater
winner. A stale atomic read can cause extra work; it cannot remove a winning
sample. The larger-ID rule for equal depth is preserved.

Tiles normally span about half the smallest circle diameter. The cache is used
on the Vulkan 64-bit atomic backend for batches of at least 4,096 points and
diameters of at least 2 px. The runtime clears it with the winner buffer and
retains it during progressive refinement. Query clipping must cover the whole
tile before a query can publish a bound. Empty queries are rejected before tile
index calculation.

The cache adds eight bytes per viewport pixel: 6.71 MiB at 1280 x 687, or
63.28 MiB at 3840 x 2160. It adds no per-model vertex buffer. It is resized and
released with the point runtime. Backend changes release unsupported buffers.
GPU resources stay outside `UiState`.

The early rejection direction is consistent with
[compute point-cloud rendering research](https://www.cg.tuwien.ac.at/research/publications/2021/SCHUETZ-2021-PCC/).
The implementation uses [Slang's atomic load](https://docs.shader-slang.org/en/latest/external/core-module-reference/types/atomic-0/load.html)
with the [Vulkan memory model](https://docs.vulkan.org/spec/latest/appendices/memorymodel.html).
SPIR-V inspection confirmed atomic loads and atomic maxima with device scope.

## Experiments and remaining cost

The early BearTrap campaign used the original 2M navigation ceiling throughout.
These are full-detail results, so the reduced cut is not responsible for the
gain. Pilot rows are exploratory measurements, not independent release claims.

| Experiment | 4 px | 8 px | Decision |
|---|---:|---:|---|
| Original | 100.27 ms | 348.27 ms | Reference |
| Sparse seed pass plus a coarse depth bound | 79.06 ms | 274.65 ms | Reject; too much work remains |
| Atomic early rejection | 33.98 ms | 74.82 ms | Keep |
| Also unroll the sample loop | 30.01 ms | 60.16 ms | Keep |
| Also cache complete tile coverage | 28.57 ms | 40.21 ms | Keep |
| Smaller tiles | 33.22 ms | 37.86 ms | Reject; 4 px regresses |
| Permute task order | 28.30 ms | 44.11 ms | Reject; no consistent gain |
| Replace tile divisions with shifts | 25.85 ms | 37.27 ms | Keep |

A points-only BearTrap control took 3.35, 10.03, and 22.83 ms at 1/4/8 px.
The same campaign's complete mesh scene took 19.53, 25.85, and 37.27 ms.
After point work is cached, the point-only frame costs about 0.3 ms, while the
mesh scene still costs about 16.5 ms. Later mesh frames cost more, as noted above.
Thus, point improvements alone do not establish a 60 Hz frame budget for this
75.8M-triangle scene.

The best next exact approach is to investigate visibility rejection for whole
point blocks and the cost of mesh surfaces and edges. It needs a conservative
depth bound over each block's complete projected footprint; testing only point
centers would lose circle edges. This work is not implemented or timed here.

A separate quality tradeoff could use one marker sample during motion and
restore four samples when the camera stops. It could retain all source points
while changing circle edges. It needs separate picking and visual tests. There
is no measured speed claim for this idea.

Before removal of the option, require acceptable p95 frame times with full
detail at the supported marker sizes, close views, larger viewports, and slower
GPUs. Use 16.7 ms for a 60 Hz target or 33.3 ms for a 30 Hz target. The current
results fail those criteria in several cases. Keeping the existing choice is
the supported decision.

## Validation and method

The Release build uses `vs2026-vcpkg` and completes without warnings. The full
CTest suite passes all 131 entries, including graphics and slow tests. New tests
cover dense atomic contention, equal-depth IDs, vertex colors, clipped and empty
queries, tile edges, later refinement, resizing, backend changes, larger prepared
navigation cuts, and full-detail selection across unequal groups.

A standalone GPU probe uses production model preparation, upload, and point
submission. It compares every 64-bit depth/ID sample with the original raster
shader. Each real model has 24 cases: 1x/4x MSAA, two camera distances, two angles,
and 1/4/8 px circles. BearTrap and the 100M scan each passed **52,761,600 sample
comparisons**, for **105,523,200 exact matches** and zero differences.
The final small-scene change affects adaptive selection only; the full-source
shader used by these probes is unchanged.

PNG export uses a separate full-source quad renderer. Its images are not proof
that the optimized compute shader is correct. The sample comparisons above
provide that evidence. Metal shader generation also passes with warnings treated
as errors. Native Apple compilation, execution, and performance were not tested.

Desktop timing uses a visible window, a requested 1280 x 720 drawable, an actual
1280 x 687 scene viewport, and 4x MSAA. Panes and helpers are hidden. Each
campaign uses one viewer process with three rounds and rotates the point-size
order. These are not three independent process launches. Each phase repeats
twelve camera views over approximately +/-6 degrees of yaw and +/-2 degrees of
pitch. The close view uses 0.35 of the fitted camera distance.

Full-detail phases have 24 recorded camera commands. Navigation phases have
72 or 96 in the large-model campaigns; the first small-model check has 48.
The runner warms up each phase and measures cached frames separately. GPU
timestamps arrive asynchronously, so the report selects completed point work.
RPC intervals and GPU times do not measure input-to-display latency. Do not
present `1000 / GPU milliseconds` as measured display FPS.

Runs exclude loading and screenshot export from timing. No build, test suite,
or second monitored viewer runs during a campaign. Guards limit private memory
to 38 GiB, require 6 GiB of free system memory, and limit a run to 20 minutes.
All archived campaigns completed without a guard violation. This controls the
test workload; it does not lock the laptop's clocks or exclude all OS activity.

## Evidence and reproduction

See the [commands and probe description](../experiments/point-navigation/README.md),
the [complete summary](benchmarks/point-navigation-20261023/summary.json), and
the [machine record](benchmarks/point-navigation-20261023/machine.json).
Each named campaign has a gzip archive with every recorded event, its round
summaries, memory guard results, and model, executable, source, and shader hashes.

Campaign names are historical labels. `final-*` is the optimized shader with the
old 2M ceiling. `quality-*` adds the 8M ceiling. `confirm-*` is the later paired
BearTrap repeat. `complete-bus` includes the small-scene correction. Source hashes
describe the runner's checkout; executable and deployed shader hashes identify
the actual retained baseline binary.

The [BearTrap sample record](benchmarks/point-navigation-20261023/validation-beartrap/samples.json)
and [100M sample record](benchmarks/point-navigation-20261023/validation-100m/samples.json)
include per-case fingerprints and mismatch counts. Each directory also contains
the probe log and guarded-run metadata. Build and test logs are retained in the
same evidence directory.
