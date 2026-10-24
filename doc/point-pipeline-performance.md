# Surface depth, GPU barriers, and a combined renderer

Measured on 23 October 2026 against
`d86e15d1d0e774e08ff0f255dce0148d3d3033ef`. This is a follow-up to
[the first point navigation investigation](point-navigation-performance.md).
The reference already includes atomic-load rejection and tile coverage caches.

Keep early surface-depth rejection for large opaque meshes. Keep the barrier
and combined-renderer changes as experiments. They did not provide a consistent
additional gain for the tested marker sizes.

The application uses the new depth path automatically. It adds no scene control
and preserves full-detail output. **Adaptive points remains off by default.**
A saved scene or view can explicitly enable it.

## Paired confirmation

The input is BearTrap: 7.11 GB on disk, 38,414,391 markers, 75,771,986 triangles,
and one group. The GPU is an RTX 3070 Laptop with 8 GiB of VRAM. The renderer is
NoGraphicsAPI Vulkan, with Slang 2026.18.3 and NVIDIA driver 616.92.

The table contains total GPU rendering time at 1280 by 687 pixels and 4x MSAA.
Each value is the median of three round p50 values. Each round has 24 measured
frames per version along a repeating 12-frame yaw cycle, after four warm frames.
Each camera frame renders both versions; their order alternates. The probe uses
the upper middle sample for each round's p50. All source markers, solid surfaces,
and triangle edges are enabled. Lower time is better.

| View | Marker diameter | Reference | Current shaders | Reduction |
|---|---:|---:|---:|---:|
| Fitted | 1 px | 18.45 ms | 18.88 ms | -2.3% |
| Fitted | 4 px | 25.16 ms | 22.72 ms | 9.7% |
| Fitted | 8 px | 35.99 ms | 29.71 ms | 17.5% |
| Close, 0.35 camera distance | 1 px | 19.25 ms | 19.28 ms | -0.2% |
| Close, 0.35 camera distance | 4 px | 28.72 ms | 22.61 ms | 21.3% |
| Close, 0.35 camera distance | 8 px | 44.60 ms | 29.44 ms | 34.0% |

The new seed is disabled at one pixel. The fitted one-pixel round ranges overlap:
18.37–19.11 ms for the reference and 18.48–19.15 ms for the current shaders.
Do not treat this small difference as a speed gain. GPU frequency and temperature
were not fixed. The paired measurements reduce time drift; they do not eliminate
it. These are GPU times, not display FPS or input latency.

A single-round 1x MSAA check also matched all final samples. At 4 and 8 pixels,
the times were 20.59 to 20.36 ms and 24.20 to 22.34 ms. This check is weaker
performance evidence than the three-round 4x MSAA campaigns.

Two other models used the same paired, three-round, 4x MSAA procedure:

| Model | Marker diameter | Reference | Current shaders | Reduction |
|---|---:|---:|---:|---:|
| Bennu, 8.93 million markers | 4 px | 10.41 ms | 7.98 ms | 23.3% |
| Bennu, 8.93 million markers | 8 px | 16.40 ms | 10.72 ms | 34.6% |
| Semantic3D, 100 million markers | 4 px | 23.89 ms | 23.84 ms | 0.2% |
| Semantic3D, 100 million markers | 8 px | 58.86 ms | 59.07 ms | -0.3% |

The point-only Semantic3D input has no opaque surface. Its seed is disabled.
Its time remains essentially unchanged. This also checks that extracting the
shared circle function for the combined prototype did not cause a large
regression in the ordinary compute path.

## Desktop application confirmation

These runs use the actual application at the same 1280 by 687 viewport and 4x
MSAA. The camera starts in an isometric view and changes both yaw and pitch.
Each version has three rounds with 24 full-detail camera frames and 72 adaptive
navigation frames per marker size. The reference and current versions run in
separate campaigns, so these results have more exposure to time drift than the
paired shader measurements. Do not treat the two tables as identical views.

| View | Diameter | Reference, full detail | Current, full detail | Reduction |
|---|---:|---:|---:|---:|
| Fitted | 1 px | 19.32 ms | 19.57 ms | -1.3% |
| Fitted | 4 px | 25.68 ms | 23.89 ms | 7.0% |
| Fitted | 8 px | 37.21 ms | 32.88 ms | 11.7% |
| Close | 4 px | 29.98 ms | 25.66 ms | 14.4% |
| Close | 8 px | 47.51 ms | 35.93 ms | 24.4% |

With adaptive points explicitly enabled, the fitted 4/8-pixel navigation times
changed from 18.67/21.85 ms to 18.36/20.74 ms. The close-view times changed from
20.06/24.50 ms to 18.87/22.07 ms. Cached frame times remained about 16–17 ms.
These numbers use the median of round medians of frames with point raster work;
cached times include all captured cached frames. The archived data also retain
frame intervals, CPU times, point counts, p95 values, and individual events.

## The three experiments

These prototype runs use the same full-source input and target. Methods and
marker sizes rotate between three rounds. They are separate from the paired
confirmation above. They include all initialization, surface, marker, barrier,
and resolve work in the GPU interval. Three rounds do not fully balance all six
method positions; use these runs to compare large effects and the paired runs
to confirm the retained change.

| Method | Fitted, 4 px | Fitted, 8 px | Close, 4 px | Close, 8 px |
|---|---:|---:|---:|---:|
| Reference | 26.80 ms | 36.88 ms | 32.06 ms | 47.27 ms |
| Precise barriers | 25.08 ms | 36.88 ms | 29.35 ms | 47.15 ms |
| Early surface depth | 22.62 ms | 32.21 ms | 23.11 ms | 30.50 ms |
| Early depth plus precise barriers | 22.91 ms | 30.91 ms | 23.41 ms | 31.34 ms |
| Combined vertex shader | 35.70 ms | 84.06 ms | 44.79 ms | 108.11 ms |
| Combined, one claim per marker | 34.92 ms | 78.70 ms | 43.46 ms | 99.59 ms |

### Early surface depth: retained

The old point pass finds the nearest marker at each sample, then tests that
marker against the solid depth in the fullscreen resolve. It can spend time on
markers that the surface will hide.

The new initialization pass reads the opaque surface depth first. It puts that
depth in the existing winner buffer with ID zero. It also initializes each
coverage tile with the lowest depth that all its samples have. Hidden marker
updates can then stop before their pixel atomics. A marker at exactly the
surface depth still wins because its original ID is positive. The resolve
discards entries whose ID is zero.

The native depth format is **D32_FLOAT**, despite the compatibility API name
`D24S8`. No 24-bit approximation is used. The change needs no additional
full-size GPU buffer or dispatch. It extends the existing point initialization.

Cached winners need care: a marker that was removed by a surface must return
when that surface disappears. The application enables seeds only when:

- There is exactly one draw-plan item: an opaque solid mesh with markers.
- It has at least one million markers, each at least four pixels in diameter.
- The backend is Vulkan with 64-bit point atomics and sampled depth.
- The target starts with clear depth. A preceding default-target view is allowed
  only if it contains clear/touch operations and no draws.
- No other draw has already been recorded in the scene view.

The existing point key tracks source, transforms, camera, and marker settings.
The depth key also tracks the mesh resource revision, eligibility, sample count,
and viewport origin. Changes reset and refill the marker cache. Mixed scenes,
transparent surfaces, small markers, small meshes, and other backends retain
the general point path. Stationary frames reuse the completed buffer.

### Precise GPU barriers: not retained in the application

The experiment replaces broad barriers with the actual producer and consumer
stages. It lets independent surface and point work overlap, then synchronizes
both before resolve. It passed the sample comparisons. However, its gain varied
by marker size and camera view, and it did not consistently improve the early
depth path. The application keeps its existing barriers.

The [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
describe these compute and graphics dependencies. Narrow scopes permit overlap;
they do not guarantee that the GPU has spare capacity to execute both workloads
at once.

### Combined renderer: rejected for general use

The prototype uses one mesh geometry pass for solid surfaces, triangle edges,
and marker winner writes. It reuses the transformed mesh position to rasterize
the full circular marker in the vertex shader. A final fullscreen resolve is
still necessary. Circles extend beyond silhouettes and keep their original IDs.
This is not a fragment-only dot effect clipped to triangle interiors.

An indexed mesh can invoke a vertex more than once. A second version uses an
atomic claim to rasterize each original marker only once. It needs a marker-ID
map and a claim buffer: about 293 MiB extra for BearTrap. The claim improved the
large-circle result but did not recover the performance of separate compute.

The combined path did help at one pixel: 19.28 to 17.97 ms in the fitted
prototype campaign. At 8 pixels it rose to 84.06 ms. The claim variant took
78.70 ms. It is unsuitable as the general renderer.

One command can still create a large amount of GPU work. The current compute
path can order compact points spatially and run their pixel updates separately
from triangle processing. Combining stages changes vertex reuse, scheduling,
and memory traffic. These measurements show the total effect; no hardware
counters were collected to assign the regression to one cause.

## Validation and reproduction

The probe compares each final MSAA **color, depth bit pattern, and original
picking ID**, not exported PNGs. The image export path uses separate quad
markers and cannot validate this change. Hidden winner-buffer entries are
expected to change when a surface seed replaces them.

The seven main probe campaigns contain **132 comparison cases and 432,645,120
sample records**, each with three fields. All fields matched exactly. The count
includes reference repeatability controls. It excludes the pilot and small GPU
fixtures. Each prototype campaign compared 126,627,840 sample records; each
four-sample paired campaign compared 42,209,280. The single-sample confirmation
compared 10,552,320.

The GPU fixtures cover authored colors, clipping, equal-depth ties, nonzero
viewport origins, and 1x/4x MSAA. Application tests cover cached frames, opacity,
solid visibility, topology-only edits, mixed scenes, earlier draws, the separate
desktop clear view, changed viewport origins, color-only targets, and
small-mesh/small-marker fallbacks.

Verification completed:

- Release build with `vs2026-vcpkg`: no compiler or shader warnings.
- Full CTest run: **132/132 passed**, including the optional experiment test.
- Follow-up after correcting the color-only test fixture's surface shader:
  **3/3 affected suites passed**. Production code did not change after the full
  run or the recorded application benchmarks.
- Vulkan core and synchronization validation: no validation warnings or errors
  in the probe self-test or the new application cache test. Logs confirm that
  both checks were active. This does not instrument every shader memory access.
- Slang Metal source generation: all nine point entries passed with warnings
  treated as errors. Native Apple compilation and runtime tests were not run.

Build logs, test logs, environment settings, and source hashes are in the
evidence directory's `validation` folder and `verification.json`. The final
application and probe binary hashes still match their confirmation campaigns.

## Keep the optional navigation reduction

The measurements do not support removing the adaptive option yet. Early depth
helps large opaque meshes, especially with larger markers and closer views.
It cannot reject against a solid surface in a point-only model. The 100-million
point input still needs about 59 ms of GPU time at eight pixels. Full-detail
BearTrap desktop frames also remain above a 16.7 ms rendering budget.

Keep full detail as the default, with adaptive navigation available through a
saved scene or explicit user choice. Further work should focus on exact
visibility rejection for point-only inputs, or an optional screen-space density
limit. A density limit changes marker output and would need a separate visual
quality decision. The combined prototype does not provide a general replacement.

The optional harness, guards, commands, and limits are in
[`experiments/point-pipeline/README.md`](../experiments/point-pipeline/README.md).
Raw evidence is in [`benchmarks/point-pipeline-20261023`](benchmarks/point-pipeline-20261023).
