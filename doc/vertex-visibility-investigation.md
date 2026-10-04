# Vertex visibility and overdraw — issue 106

Shader edges are integrated in `c58a9de`. The next bottleneck is redundant marker
work: a large fraction of the submitted circles never contribute a final sample.
The investigation lives in the opt-in overlay experiment; the viewer's vertex
rendering has not changed.

## What was compared

The experiment uses the production circle and packed picking-ID shaders at
1280 × 720, 4× MSAA, with the picking attachment enabled. It keeps original
geometry, marker order, circle sizes, and original marker IDs. Opaque surfaces
use native shader edges. Every candidate is compared with the original draw,
including **every MSAA picking sample**, not just the resolved color.

Five draw inputs isolate different kinds of wasted work:

- **All:** submit every marker, as the viewer does now.
- **Frustum:** remove only circles wholly outside the view or depth interval.
- **Conservative:** also reject a marker when its complete padded square
  footprint is behind opaque depth. A reversed-depth minimum pyramid includes
  every MSAA sample; background and uncertain coverage keep the marker.
- **Center:** test only the center. This is an intentionally unsafe control.
- **Oracle:** retain exactly the original marker IDs found in the baseline's
  final MSAA image, preserving their original order.

Depth readback, CPU classification, selection construction, and GPU upload happen
**outside the timed frames**. These are drawing-cost experiments, not measured
end-to-end improvements. The oracle requires the complete original render first;
its cheap subsequent draw is an upper bound on the opportunity, not a usable
visibility algorithm. The CPU reference is also unsuitable for the render thread.
The conservative pyramid can retain hidden markers, so its rejection count is
not the exact number of markers hidden by surfaces.

## Measured results

Release Vulkan on the RTX 3070 Laptop 8 GiB, driver 616.92. Values below are the
median of three round medians. Total GPU scene time includes surfaces, shader
edges, markers, and resolve; it excludes the selection work described above.

| Scene / camera / marker size | All markers | Conservative surface culling | Known final contributors (oracle) |
| --- | ---: | ---: | ---: |
| BearTrap fitted, 1 px | 59.87 ms | 40.86 ms | 15.50 ms |
| BearTrap fitted, 4 px | 100.80 ms | 87.29 ms | 15.61 ms |
| BearTrap fitted, 8 px | 197.11 ms | 187.91 ms | 15.83 ms |
| BearTrap close (0.35× distance), 4 px | 74.83 ms | 44.52 ms | 17.26 ms |
| San Miguel fitted, 4 px | 17.05 ms | 7.74 ms | 1.97 ms |

The tested conservative filter removes about 13% of drawing time on fitted
BearTrap at four pixels, 41% in the closer view, and 55% on fitted San Miguel.
Its real benefit will be smaller after adding GPU classification and compaction.
The closer run varied more: original round medians were 70.50–76.92 ms and
conservative round medians were 43.42–47.35 ms. Small frustum-only differences
are not a demonstrated improvement.

BearTrap submits 38,414,391 markers. At the fitted camera, all of their footprints
are on screen. With four-pixel markers, conservative surface culling retains
27,632,178 markers, while only 88,768 original IDs contribute to the final image.
The vertex-only control has the same 88,768 contributors, with no opaque surfaces
available for culling.

BearTrap's vertex-only GPU times are 46.01 / 100.24 / 222.16 ms at 1 / 4 / 8 pixels.
The marker count and quad expansion stay constant while footprint area increases.
That is evidence of substantial raster/coverage/depth cost in addition to the
vertex-processing cost; these measurements do not separate those hardware stages.
With the 88,768 known contributors, the four-pixel marker/resolve draw takes
0.09 ms. This excludes the work needed to discover those contributors.

San Miguel submits 9,021,669 markers: the conservative filter retains 2,226,156,
while only 3,233 contribute to the combined fitted image. Without surfaces,
8,915 contribute and the original point draw takes 18.90 ms. Surface occlusion
therefore matters substantially there, but marker-on-marker redundancy remains.

The current quad path submits four vertex shader invocations and two triangles
per marker: about 154 million vertex invocations and 77 million triangles per
frame for BearTrap. At four pixels, the projected square footprints sum to about
615 million pixel areas, concentrated into roughly 50,000 final marker pixels.
These are logical workload and projected-area counts, **not measured DRAM traffic
or fragment shader invocation counters**; caches and early rejection affect the
actual hardware work.

Testing only marker centers is insufficient to preserve the current behavior.
On fitted BearTrap at four pixels, it changes 11 resolved color pixels and 5,145
picking samples. A nearly identical screenshot can therefore conceal a picking
regression. Synthetic tests also cover circles whose centers are offscreen or
behind an occluder while part of their circle remains visible.

## Next implementation experiment

The larger opportunity is to avoid expanding and drawing markers that cannot win
any covered sample, including markers hidden by other markers. Merely testing
against opaque surfaces leaves substantial redundant work.

First, implement and time **current-frame GPU footprint culling and compaction**
with indirect draws and original IDs. The reference shows enough potential on
San Miguel and closer BearTrap to justify that experiment. Include its full
pyramid, classification, compaction, and synchronization cost in the comparison;
the fitted eight-pixel BearTrap case has only about 5% draw-time headroom from
this filter and can easily lose that gain to extra work.

The larger follow-up prototype is a GPU path that projects each center once, bins
candidate circles into bounded screen tiles, and resolves circle coverage,
depth, and original IDs within each tile. It must preserve the current equal-depth
submission order and per-sample provenance, retain complete circles at viewport
and occluder boundaries, and fall back safely when tile capacity is exceeded.
Dense tiles need a hierarchy or conservative depth rejection so that binning does
not simply move the same overdraw into a compute loop. This is a proposed design,
not a demonstrated performance result.

Use original marker IDs throughout; never reuse a
visibility list after its camera, transform, geometry, point size, or viewport
inputs change. Transparency needs a separate policy because the final-ID oracle
does not preserve all alpha-blended contributions.

## Validation and reproduction

[CSV measurements](benchmarks/vertex-visibility-20261004/results.csv) and
[JSON evidence](benchmarks/vertex-visibility-20261004/results.json) retain 50
aggregates across ten scenarios, all three round medians, camera matrices,
source/model/executable hashes, capture hashes, and resource guards. The JSON
links the raw per-frame results under
`D:/.worktree/woby-overlay-build/measurements/vertex-*`.

All three guarded runs completed without a concurrent Woby workload. Peak private
memory stayed at or below 12.18 GiB and available memory at or above 37.69 GiB.
All 120 reference/frustum/conservative/oracle comparisons preserved every color
byte and picking sample. The other 30 comparisons are the deliberately unsafe
center-only control. Baseline and filtered captures were saved outside timing;
representative originals are retained alongside the CSV/JSON evidence.

Debug and Release experiment builds are warning-free. The three experiment CTest
entries pass in both configurations. The new tests cover invalid depth/provenance,
depth clipping, viewport fringes, opaque occluders, duplicate marker positions,
and complete color/ID parity at 1/4/8/40 pixels with one and four samples.

Use the build instructions in
[the experiment README](../experiments/mesh-overlays/README.md), then run serially
with a fresh output directory and no concurrent Woby workloads:

```powershell
uv run experiments/mesh-overlays/run.py `
  D:/.worktree/woby-overlay-build/bin/Release/woby_overlay_prototype.exe `
  D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj `
  D:/.worktree/woby-overlay-build/measurements/vertex-new `
  --visibility --rounds 3 --seconds 1 --warmup 0.5
```

`--visibility` enables all-sample picking capture. It measures 1/4/8-pixel markers
both with surfaces/edges and alone. `--point-size 4`, `--solid 1`, `--zoom 0.35`,
and `--orthographic` allow focused controls. Each variant warms for at least 12
frames, then measures at least 20 frames and the requested duration. Method order
rotates across rounds. Capture/classification results are recorded separately
from GPU total, surface, marker/resolve, and CPU submission samples. The existing
38 GiB private-memory / 6 GiB available-memory and workload-overlap guards remain
in force.
