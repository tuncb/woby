# Point pipeline experiments

This probe compares three changes with the renderer at
`d86e15d1d0e774e08ff0f255dce0148d3d3033ef`:

1. Seed point depth from the opaque surface before point rasterization.
2. Use compute-to-compute and producer-to-fragment barriers. Allow independent
   surface and point work to overlap until the resolve needs both results.
3. Rasterize vertex circles in the mesh vertex shader. Reuse the transformed
   vertex and the production circle raster function. Test both repeated work
   and an atomic claim that permits one raster operation per original point.

The combined method draws the solid and triangle edges with the normal fragment
shader. It writes point winners from the vertex shader, then resolves them in
a separate fullscreen draw. It preserves circles outside triangle boundaries.
It does not replace them with dots clipped to triangle fragments.

The additional `production` method uses the current application clear and raster
shaders. The other methods load the retained reference shaders. All methods use
full source points. There is no navigation reduction.

## Build

Keep the reference application's executable, DLLs, and `assets` directory before
you build a change. Use `D:/.worktree` for a separate Windows checkout if needed.

```powershell
$env:VCPKG_ROOT='C:/work/vcpkg'
cmake --preset vs2026-vcpkg -DWOBY_BUILD_POINT_PIPELINE=ON
cmake --build --preset vs2026-vcpkg --config Release
ctest --test-dir build/vs2026-vcpkg -C Release -R woby_point_pipeline_gpu --output-on-failure
```

This optional target runs on Windows Vulkan. It needs 64-bit buffer atomics,
fragment barycentrics, and GPU timestamps. It is off in a normal build.

## Run

Use a new output directory. Run campaigns, builds, and tests separately.

```powershell
uv run experiments/point-pipeline/run.py build/vs2026-vcpkg/bin/Release/woby_point_pipeline.exe D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj build/point-pipeline-reference-app/assets/shaders/spirv build/pipeline-new --frames 24 --rounds 3
```

Use `--zoom 0.35` for a close view and `--samples 1` to check single sampling.
Use `--paired` to alternate the reference and current production shaders at each
identical camera position. The first method alternates on each frame. This
confirmation run excludes the slower combined methods and reduces time drift
between the two measurements.
The default target is 1280 by 687 pixels with 4x MSAA. A round rotates marker
sizes and method order. Each method gets four warm frames, followed by the same
camera sequence. GPU timestamps include surface drawing, point initialization,
point drawing, barriers, and resolve. Capture and readback are outside the timed
interval. Each frame completes before the next frame starts. These times are
not display FPS or application latency measurements.

The runner records source, executable, model, and shader hashes. It aborts if
another monitored workload starts, process private memory exceeds 38 GiB,
available memory falls below 6 GiB, or the run exceeds 20 minutes. All working
paths are absolute. A unique temporary working directory is removed on success
or failure.

## Correctness

Before timing, each method renders two camera angles at 1, 4, and 8 pixels.
The probe reads each final MSAA color, original picking ID, and D32 depth sample.
It compares all three fields exactly with the reference. It does not compare
hidden point-buffer entries: a depth seed intentionally replaces those entries
with a surface key. A mismatch makes the run fail after the evidence is saved.

The GPU self-test adds nonzero viewport origins, authored colors and missing
colors, clipping, overlapping surfaces, and equal-depth points at 1x and 4x MSAA.
Application GPU tests separately check cached frames and changes to opacity,
solid mode, topology, and other scene draws.

## Limits

- The combined prototype requires one triangle group. It keeps the source IDs
  with a vertex-to-marker map. The optional claim buffer adds four bytes per
  marker. Neither buffer is added to the application renderer.
- The probe resets every frame. It does not model progressive refinement,
  translucent surfaces, presentation, or the UI. The application runner in
  `tests/point_render_benchmark.py` measures those application frame stages and
  cached/refining point states separately.
- The application enables surface seeds only for one opaque mesh with at least
  one million markers and a marker diameter of at least four pixels. There must be no
  earlier draw in that depth-cleared view. Its point key and mesh resource
  revision cover all occluder inputs. Other scenes use the general point path.
- GPU frequency and temperature are not fixed. Use the raw round ranges and
  compare methods within a campaign. Do not compare older campaigns as though
  all hardware conditions were identical.

Vulkan's [synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
describe the compute and graphics dependencies used in the precise-barrier
variant. The [synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
defines the execution and memory scopes.
