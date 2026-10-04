# Mesh overlay prototypes (issue 106)

Opt-in, fixed-resolution Vulkan experiments that established the edge integration
in the viewer. The experiment uses
the real OBJ loader, mesh preparation, camera math, vertex layout, and mesh,
circle, and packed marker-ID shaders. A small direct NoGraphicsAPI harness adds
GPU timestamps and alternate submission strategies.

| Method | Submission / edge representation |
|---|---|
| `legacy` | Original group order: solid, unconditional lines, depth-tested circles. |
| `ordered` | All surfaces, then depth-tested lines, then circles. Opaque lines write depth so crossing silhouette fragments select the nearest edge. |
| `barycentric` | Native fragment barycentrics color triangle edges during surface drawing. No separate line draw for ordinary opaque solid+edge groups. |
| `pulled` | Same edge shading, but a nonindexed vertex shader reads the original triangle index buffer and generates barycentric coordinates. No expanded vertex buffer; indexed vertex reuse is lost. |

`edges` means hidden-line rendering for the new methods: render depth without
surface color, then the edges. `xray` uses the line representation without depth
testing, followed by the existing depth-tested circles. A shader edge is shaded
inside its owning triangle. Silhouette coverage and antialiasing therefore differ
from centered hardware lines. Both sides of a shared edge are still represented.
This is not a screen-space outline filter and does not remove coplanar edges.

The optional native capability is now provided by the production backend.
Unsupported devices explicitly omit `barycentric`; requesting it fails. The
fallback remains separately selectable and measurable. The dependency patch
applies only inside CMake's private FetchContent tree. Metal source translation
is possible with Slang; this Windows experiment does not validate Metal runtime
behavior. The viewer uses the pulled fallback on Metal.

## Build and check

Use the repository's Visual Studio development preset and its normal toolchain:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_OVERLAY_PROTOTYPE=ON -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure
cmake --build build/vs2026-vcpkg --config Release --target woby_overlay_prototype woby_overlay_tests --parallel 2
```

The recorded run used Slang 2026.18.3, selected through `-DWOBY_SLANGC=...`.
An older `slangc` bundled in a Vulkan SDK may lack the named barycentric
capability required by this experiment.

The tests cover invalid workloads, opaque group order, hidden-line versus X-ray,
circle sizes 1/4/8/40, clipped/transformed markers, marker provenance, transparency,
and native versus pulled edge shading at one and four samples. They are GPU
render-and-readback tests, not shader-text assertions. `woby_overlay_logic` can run
without a GPU; `woby_overlay_gpu` is registered with `WOBY_TEST_HEADLESS=ON`.

## Measure

Run serially, with builds and other benchmarks stopped. Always use a new output
directory. The wrapper retains the campaign's 38 GiB private-memory / 6 GiB
available-memory guards, sampled every 200 ms. These can overshoot and are not
hard allocation caps.
The wrapper also rejects overlapping Woby viewer/test/benchmark processes; it
terminates only its own experiment if another such workload starts during a run.
It hashes the model before launching, so recorded load times have a warmed file
cache and must not be interpreted as cold-load measurements.

```powershell
uv run experiments/mesh-overlays/run.py `
  build/vs2026-vcpkg/bin/Release/woby_overlay_prototype.exe `
  D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj `
  build/overlay-beartrap-new --samples 4 --ids 0 --rounds 3 --seconds 2 --warmup 1
```

Default scenarios are solid, edges, solid+edges, vertices at 1/4/8 pixels,
combined, X-ray, and opacity 0.4. Use `--scenario combined`, `--method ordered`,
`--ids 1`, `--zoom 0.5`, or `--orthographic` for focused controls. Dimensions are
actual offscreen target dimensions, default 1280x720, independent of DPI/window
placement. Defaults stay within production size limits; maximum target size is
1920x1080. `--fixture --output NEW_DIRECTORY` on the executable captures the small
occlusion fixture instead of loading an OBJ.

Each round rotates method order. Every case warms up for at least 12 frames and
the requested warm-up duration, then measures at least 20 frames and the requested
duration. JSON retains every total GPU and CPU submission sample, median pass
times, input geometry counts, device, dimensions, camera matrix, and capture
coverage. Captures and GPU-ID readback run outside measurement windows. Empty
captures fail the run. Inspect the PNGs as well as their coverage counts.

Ordinary overlay pass timestamps are within one render pass; the final marker interval includes
color resolve/end-pass work. Legacy draws are interleaved, so only their total
GPU time is reported. CPU submission excludes fence waits. The harness completes
each frame before reusing resources: these are scene GPU timings, not viewer FPS
or end-to-end interaction latency. ID mode measures the extra production-style
attachment, not the full hover lookup/highlight/UI workflow.

All methods retain the same original geometry, marker IDs, and control edge
buffer in a process. This isolates drawing costs from residency changes. The
native path does not use that edge buffer for visible surface edges; its possible
memory saving is reported as a calculated payload, not a measured VRAM reduction.
The default edge comparison does not decimate geometry, sample markers, or cull
their visibility. The separate visibility diagnostic below constructs filtered
draw lists outside its timed frames.
Model colors are deterministic per group; global opacity is the transparency
control. Arbitrary saved scenes, imported lines, detectors, and interleaved opaque
and transparent groups remain production-integration work.

## Current-frame GPU culling and compaction

`--gpu-culling` compares the original marker draw (`all`), a render-pass break
without filtering (`split`), GPU frustum culling, and GPU full-footprint culling.
It defaults to picking IDs enabled; `--ids 0` measures without that attachment.
`--point-size 4` and `--solid 1` select one workload; omitted values exercise
1/4/8-pixel markers both with surfaces and in a points-only view.

```powershell
uv run experiments/mesh-overlays/run.py `
  D:/.worktree/woby-overlay-build/bin/Release/woby_overlay_prototype.exe `
  D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj `
  D:/.worktree/woby-overlay-build/measurements/gpu-culling-new `
  --gpu-culling --point-size 4 --rounds 3 --seconds 1 --warmup 0.5
```

Every frame stores opaque depth, builds a reversed-depth minimum pyramid from
all MSAA samples, classifies complete padded marker squares, computes an ordered
hierarchical prefix scan, scatters original marker offsets, and generates
per-group indirect draw arguments. The marker pass loads the same attachments
and resolves color only at its end. No CPU visibility readback or selection
upload is needed to draw. The padding and full square are conservative for the
circle shader; background depth keeps uncertain silhouettes. Transparent views
and points-only views skip depth preparation and use frustum rejection only.

Stable compaction preserves original group/marker order, equal-depth winners,
alpha contributions, and picking IDs. Each group has a separate output partition,
including when groups share an input range. GPU buffers, block descriptors, and
derived matrices are reused until their inputs change; visibility is recomputed
from the current frame and never reused across camera/transform changes.

The total GPU timestamp includes attachment clear/store/load, pyramid generation,
classification, scan, scatter, synchronization, indirect drawing, and resolve.
JSON stores per-frame stage timings, group counts, and exact RGBA/all-sample ID
comparisons. Captures and diagnostic count readback are excluded. Buffer byte
counts describe requested payload, including count readback; they are not a
measurement of total committed VRAM. CPU submission excludes resource/pipeline
setup and fence waits. This remains an opt-in Vulkan prototype, outside the viewer.

GPU tests compare rendered colors and every picking sample at 1/4x MSAA, with and
without IDs, against both original drawing and independent CPU selections. They
cover sizes 1/4/8/40, non-power-of-two and one-pixel targets, complete-circle
fringes, transparency, transforms, camera changes, visibility changes, reordered
and duplicated groups, zero/all retained outputs, partial blocks, and all three
prefix levels (a group crosses global block 65536).
Fused surface/edge shading blends transparency once; the separate surface and
line passes can blend twice. Transparent images are therefore a diagnostic
comparison, not a claim of equivalent appearance or a production replacement.

Aggregate completed runs and check unchanged-mode image parity with:

```powershell
uv run experiments/mesh-overlays/analyze.py build/overlay-summary-new `
  build/overlay-beartrap-new
```

See [the measured results](../../doc/overlay-prototype-results.md) for the
initial five-model comparison and the recommended integration scope.

## Vertex visibility diagnostic

Add `--visibility` to run the offline marker-visibility investigation. It enables
all-sample ID capture and compares all markers, whole-footprint frustum culling,
conservative opaque-depth culling, a deliberately unsafe center-only test, and
the set of original IDs that contribute to the baseline's final MSAA image.
The latter is called `oracle`: discovering that list requires the original draw,
so its timing is an upper bound on potential savings, not an implemented speedup.

All readback, CPU classification, selection construction, and upload are excluded
from timed frames. The marker shader reads the original marker offset through
the compacted list, preserving order and provenance. Every preserving variant
must match the baseline's RGBA bytes and all picking samples or the run fails.
This experiment covers opaque markers; final IDs do not encode every blended
contribution from transparent markers.

By default it tests 1/4/8-pixel markers with surfaces/edges and alone. Use
`--point-size 4`, `--solid 1`, `--zoom 0.35`, or `--orthographic` for controls. The
same serial-run and memory guards apply. See the
[investigation and measured results](../../doc/vertex-visibility-investigation.md)
for reproduction, limits, and the proposed next GPU prototypes.

References: [issue 106](https://github.com/tuncb/woby/issues/106),
[Khronos barycentric sample](https://docs.vulkan.org/samples/latest/samples/extensions/fragment_shader_barycentric/README.html).

## Compact opaque point renderer

The same opt-in build now includes `woby_point_prototype`. This is a separate
headless architecture experiment; Woby's production vertex renderer is unchanged.
It requires Windows Vulkan with 64-bit buffer atomics. The optional device
feature is queried and enabled only in builds with this experiment enabled.

```powershell
cmake --build D:/.worktree/woby-overlay-build --config Release --target woby_point_prototype woby_overlay_tests --parallel 2
uv run experiments/mesh-overlays/run.py D:/.worktree/woby-overlay-build/bin/Release/woby_point_prototype.exe D:/temp/obj_tests/pointclouds/semantic3d_sg27_station8_100000000_xyz_points.obj D:/.worktree/woby-overlay-build/measurements/points-new --mode compute --samples 4 --rounds 3 --frames 12 --navigation-frames 90
```

Use a new output directory for every run. `--mode control` measures the original
full-data quad renderer with the same camera, point sizes, sample count, and
opaque surface setting. `--point-size 4` selects one size (otherwise 1/4/8),
`--zoom 0.25` moves closer, and `--solid 1` includes opaque mesh surfaces.
The runner rejects overlapping viewer, test, and active build workloads and
records source, executable, input, and SPIR-V hashes with memory guards.

The compute mode reports four separate workloads:

- Full-data rendering without LOD, clearing and processing all points each frame.
- A deterministic 90-frame orbit with spatial detail selection, a maximum of
  two million submitted points, and feedback toward an 8 ms GPU frame target.
  `--budget` and `--target-ms` change those limits. The target is feedback, not
  a hard real-time guarantee; CPU traversal and fixed surface work also matter.
- Stationary refinement, replaying every original point in bounded batches into
  persistent depth/ID winners. The final RGBA and every sample ID must exactly
  match the full compute reference. View, size, surface mode, group visibility,
  transform, or color changes invalidate the accumulation.
- A fully refined stationary view using cached visibility.

The CPU hierarchy holds Morton-ordered, unquantized positions and original IDs,
with spatially stratified proxy samples and actual extrema. GPU point allocations
contain 12-byte positions plus 4-byte IDs, in independently allocated chunks with
bounded staging. The visibility buffer holds an atomic 64-bit depth/ID winner
per sample. Full circle footprints compete against opaque surface depth; shading
then runs for winning samples. Source IDs provide deterministic equal-depth ties
and map back to the retained mesh's precise positions. CPU picking traverses
original leaves, never LOD proxies. The benchmark queries covered samples and
checks cloud-only results against the full GPU reference.

This prototype evaluates circles at each sample. The production quad control
discards its circle at pixel frequency, so their MSAA edge coverage is different.
Navigation coverage is measured against the new full-data compute reference;
it is approximate, and the recorded ratio is not a guarantee for arbitrary
thin structures. No hole filling or averaged replacement positions are used.

All compact data is resident. The existing OBJ/Mesh loader and its CPU copies
remain as an adapter; this is not yet an out-of-core importer, a Metal fallback,
an asynchronous UI integration, or a replacement for source-data ownership.
Hierarchy construction is measured as offline setup. All geometry is opaque.
The shader visits footprint samples directly; tiled visibility and coordinate
quantization are further experiments, not hidden assumptions in these results.

See [the measurements and integration limits](../../doc/point-cloud-prototype.md).
