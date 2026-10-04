# GPU vertex footprint culling and compaction — issue 106

The opt-in overlay prototype now performs conservative current-frame marker
culling, stable compaction, and indirect drawing entirely on the GPU. It keeps
the original complete circles and picking IDs. The viewer's vertex path is
unchanged; this experiment measures the implementation before integration.

## Pipeline

1. Draw surfaces and edges, store their color, picking, and opaque depth
   attachments, and postpone the MSAA color resolve.
2. Reduce every depth sample to a reversed-depth minimum pyramid. Build only
   the levels needed for the selected circle size. Background and incomplete
   coverage prevent rejection; non-power-of-two padding is conservative.
3. Classify each marker's complete padded square footprint. This contains its
   circle and an extra half-pixel margin. Reject it only when the whole footprint
   is outside the view/depth interval or behind opaque depth, with a depth
   tolerance. A hidden center alone is insufficient.
4. Count retained markers in blocks of 256, perform a hierarchical ordered
   exclusive scan, and scatter original marker offsets into per-group output
   partitions. Write each group's indirect instance count on the GPU.
5. Synchronize compute writes with indirect/vertex reads, load the existing
   attachments, draw the compacted circles, and resolve color.

Stable ordering preserves equal-depth winners and alpha contributions. Separate
output partitions also support groups that share an input range but have
different transforms. No append-atomic ordering, CPU visibility readback, or
CPU selection upload is needed to draw. The existing compacted vertex shader
performs an extra index lookup to retain original packed picking IDs.

GPU buffers and block layout are cached for unchanged geometry/ranges. Derived
matrices refresh when transforms or the camera change; every frame recomputes
visibility from its own depth. The prototype drains each frame before reusing
buffers. Production integration still needs the viewer's resource lifetime and
frames-in-flight handling.

With no opaque occluders, the footprint variant skips depth preparation and uses
frustum checks only. Transparency tests use the harness's global opacity;
arbitrary mixed-opacity saved scenes are outside this prototype's scope.

## Measurement

The controls are original drawing (`all`), a render-pass split with no culling
(`split`), GPU frustum-only culling, and GPU footprint culling. Total GPU time
includes clear/store/load, depth preparation, classification, scan, scatter,
barriers, indirect drawing, and final resolve. Counter/image readback is outside
the interval. Initial allocation and pipeline compilation happen before timing.
These are headless scene GPU timings, not viewer FPS or interaction latency.

Release Vulkan, RTX 3070 Laptop 8 GiB, driver 616.92, Ryzen 5800H, 64 GiB RAM.
All cases use 1280 × 720 and 4× MSAA. The picking attachment is enabled except
for the explicitly named control. Each result uses three rounds with rotating
method order, at least 12 warm-up frames/0.5 seconds and 20 measured frames/one
second per variant. Comparisons use their own run's baseline; the older CPU
selection timings exclude culling cost and are not directly comparable.

Times are medians of the three round medians. `GPU culling` below includes its
entire GPU cost, not just the filtered draw.

| Scene / camera / circle size | Original | Split only | GPU culling | Culling stage | Total reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| BearTrap fitted, 1 px | 64.32 ms | 64.27 ms | 49.34 ms | 4.59 ms | 23.3% |
| BearTrap fitted, 4 px | 112.92 ms | 105.50 ms | 103.98 ms | 4.76 ms | 7.9% |
| BearTrap fitted, 8 px | 219.85 ms | 206.24 ms | 215.61 ms | 4.93 ms | 1.9%* |
| BearTrap close (0.35× distance), 4 px | 71.17 ms | 70.71 ms | 50.07 ms | 4.53 ms | 29.6% |
| San Miguel fitted, 4 px | 15.86 ms | 15.77 ms | 8.33 ms | 1.15 ms | 47.5% |
| San Miguel fitted, 4 px, IDs off | 15.84 ms | 15.84 ms | 8.31 ms | 1.15 ms | 47.5% |

\* The eight-pixel fitted result is **not a reliable demonstrated gain**: culling
round medians span 202.47–215.87 ms and the split control spans 205.91–219.50 ms.
The split control also varies materially in fitted four-pixel BearTrap
(105.25–113.79 ms), while culling spans 97.10–104.12 ms. The four-pixel original
round medians are 107.52–113.73 ms. Report these small fitted-view benefits with
their controls/ranges; there is no evidence here for universally enabling this
path. Close-up BearTrap culling spans 49.67–50.19 ms versus original
65.64–71.19 ms. San Miguel culling spans 8.30–8.84 ms versus 15.78–16.27 ms.

BearTrap submits 38,414,391 markers. The GPU retains 21,760,825 / 27,632,170 /
31,888,561 at fitted 1/4/8-pixel sizes, and 14,459,945 in the close four-pixel
view. San Miguel retains 2,226,156 of 9,021,669. The GPU count differs from the
earlier CPU reference by eight markers in fitted four-pixel BearTrap; the two
classifiers need not select the identical conservative superset. Complete
rendered color and picking results remain identical.

Classification dominates culling cost: about 3.7–4.0 ms on BearTrap and 0.9 ms
on San Miguel. The ordered block-count scan/argument stage is about 0.02 ms and
0.01 ms respectively. Scatter takes about 0.8–0.9 ms on BearTrap and 0.2 ms on
San Miguel; the small depth pyramid is about 0.04–0.05 ms. These stage medians
need not sum exactly to the separately computed total median. The full culling
interval also includes the pass transition and synchronization.

CPU submission at fitted four pixels increases from 0.21 to 0.33 ms on BearTrap
and from 3.36 to 3.96 ms on San Miguel (one versus 2,203 drawing groups). These
CPU measurements include recording/submission and cached-input checks, excluding
resource/pipeline setup and fence waits. They are reported separately from GPU
time; the measured GPU reduction is not a claim about full viewer frame rate.

The additional requested buffer payload is about **300 MiB for BearTrap** and
**74 MiB for San Miguel**. This includes the minimum-depth pyramid, stable rank
scratch, compacted marker offsets, metadata, scan levels, indirect arguments,
and diagnostic count readback. It is not committed-VRAM telemetry. All variants
retain those allocations so memory pressure is comparable. The experiment also
retains the original edge buffer for its older controls.

With surfaces disabled, this algorithm has no opaque depth to reject against.
At fitted four pixels, BearTrap gets slower from 104.68 to 110.54 ms and San Miguel
from 18.12 to 19.39 ms. Close-up BearTrap only removes 753,139 offscreen markers,
and still gets slower from 68.18 to 74.00 ms. Frustum-only compaction likewise
does not demonstrate a reliable improvement in any tested case. Disabling the
picking attachment on San Miguel barely changes its outcome; it is not the
main source of this workload's cost.

## Integration direction

The prototype establishes a useful GPU path for scenes with substantial opaque
surface occlusion. Integration should be selective: preserve direct drawing for
points-only/translucent views and workloads where little can be rejected. A
surface being present alone does not guarantee a net gain, as the large fitted
circles demonstrate. Avoid a hard-coded marker-count threshold based only on
these two models; retain the direct path and measure the viewer's full frame.

This is also not a solution to marker-on-marker overdraw. Even after safe
surface culling, fitted BearTrap at four pixels spends about 82.79 ms drawing
markers and resolving color. The earlier final-contributor study found only
88,768 contributing IDs there, versus 27.6 million markers this path retains.
The remaining opportunity is the proposed circle/tile visibility prototype,
which must preserve every circle sample and equal-depth picking order without
first drawing all markers. It has not been implemented in this change.

## Validation

All four guarded campaigns completed with no overlapping Woby workload. Peak
private memory stayed at or below 12.18 GiB, available memory at or above
36.25 GiB. All 144 color comparisons matched byte-for-byte; all MSAA picking
samples also matched in the 120 comparisons with IDs enabled. The remaining
24 comparisons intentionally had no picking attachment. Counts stayed identical
across rounds. This validates these workloads, not every possible scene/GPU.

[CSV measurements](benchmarks/gpu-vertex-culling-20261004/results.csv) and
[JSON evidence](benchmarks/gpu-vertex-culling-20261004/results.json) retain 48
aggregates, all three round medians for every stage, frame counts, camera
matrices, source/model/executable/SPIR-V hashes, capture hashes, resource guards,
and paths to the raw per-frame results. Representative captures are retained
in the same folder. All runs used the same source and executable hashes.

The full Debug suite passed all 918 CTest entries. Final focused experiment
checks passed in Debug and Release. Debug also passed with
`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`, with no validation messages. The
Debug application/full build and both experiment configurations are warning-free;
Slang compilation treats warnings as errors and validates generated SPIR-V.

New GPU tests compare full RGBA and every MSAA picking sample with the original
draw, and compare the compacted list and per-group counts with independent CPU
selections on synthetic fixtures. Coverage includes:

- Complete-circle fringes outside the viewport and around opaque silhouettes;
  1/4/8/40-pixel markers, one/four samples, and IDs enabled/disabled.
- Non-power-of-two and one-pixel viewports, depth clipping, transparency,
  equal-depth duplicate positions, and edge-only/X-ray views.
- Camera/model changes, visibility changes, group reordering, duplicated input
  ranges, empty groups, and zero/all retained markers.
- Partial 256-marker blocks, two-level scans, and a group crossing global block
  65,536 through three scan levels. Original ID order is checked directly.

The implementation and reproduction commands are in the
[experiment README](../experiments/mesh-overlays/README.md#current-frame-gpu-culling-and-compaction).
This prototype is validated on Windows Vulkan; Metal runtime is untested.
The [previous investigation](vertex-visibility-investigation.md) explains the
remaining marker-on-marker overdraw opportunity and why center-only culling
does not preserve picking.
