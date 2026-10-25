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

## Shader audit

The same optional target also accepts `--audit CONFIGURATION OUTPUT`. Use
`shader_audit.py` to create the configuration, record input hashes, and apply
the resource guards. This mode compares complete shader directories within
one process. It does not change the application's shader files.

Keep the four files from `shaders/native` and the SPIR-V directory from the
same reference build. Build candidates into a new directory:

```powershell
uv run experiments/point-pipeline/shader_variants.py build/reference-source build/reference-app/assets/shaders/spirv build/shader-candidates --compiler C:/tools/slang/2026.18.3/bin/slangc.exe --ngapi build/vs2026-vcpkg/_deps/woby_ngapi-src --validator C:/VulkanSDK/1.4.357.0/Bin/spirv-val.exe --variants threads256 sample-generic split-vertices early-tests
uv run experiments/point-pipeline/shader_audit.py build/vs2026-vcpkg/bin/Release/woby_point_pipeline.exe D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj build/reference-app/assets/shaders/spirv build/shader-candidates build/shader-audit-new --variants repeat threads256 sample-generic --workloads combined:1 combined:4 combined:8 points:8 --frames 24 --rounds 4
uv run experiments/point-pipeline/audit_summary.py build/shader-audit-new --output build/shader-audit-summary.json
```

Use `fixture` in place of the model path for a small, four-group correctness
case. It includes authored and missing colors, depth ties, clipped triangles,
and a nonzero viewport origin. Test it at both `--samples 1` and `--samples 4`.
The CTest entries `woby_shader_audit_tools` and `woby_shader_audit_gpu` check the
reporting rules and baseline/repeat rendering. The GPU test requires the optional
target and `WOBY_TEST_HEADLESS=ON`.

Supported workloads are `solid`, `edges`, `pulled`, `lines`, `line-sprites`,
`uv`, `comparison`, `quads`, `points`, `combined`, `transparent`,
`transparent-edges`, and `transparent-pulled`. A suffix such as `points:8`
sets marker diameter in pixels. `combined` draws the solid, native triangle
edges, and compute points. All point modes use full source detail. Both sides
apply the same production surface-depth seed rules.

Each camera frame runs all candidates in a rotating order. There are four warm
frames per round. GPU timestamps include all rendering passes, clears, barriers,
and resolves. They exclude capture and readback. The summary pairs individual
camera-frame times before it calculates ratios. It reports the median of round
medians and their range. These times are not desktop FPS. Keep `repeat` as a
control: it uses exactly the reference shaders and exposes measurement noise.
Identical shader bytecode shares a driver pipeline within the audit process.
The generator's source anchors target the reference at `f83fb21`. Use its saved
sources, including the headers, when reproducing this audit. The current
application already contains `sample-generic`.

Before timing each workload, the probe compares every final color, D32 depth,
and picking-ID sample at two camera angles. A mismatch fails the run by default.
Use `--allow-mismatch` only to finish a screening campaign; the report still marks
that candidate as incorrect. Incomplete campaigns cannot enter the summary.
The runner retains configuration, hashes, raw frame times, validation counts,
resource limits, exit status, and progress logs. Use a new output directory for
each run. Do not run a campaign at the same time as builds or other GPU work.

The candidate generator contains research variants, not approved application
settings. In particular:

- `identity-normal` is valid only for an identity model transform.
- `early-tests` tests opaque fills. Applying it to hidden-line or zero-alpha
  draws can change depth. Production use would need separate shader entries.
- `split-vertices` uploads extra position/normal and UV streams. The probe uses
  a spare root pointer for UVs; production use would need a proper buffer and ABI.
- `split-conditional` combines those streams with conditional UV reads. It
  preserves the separate comparison attributes when the UV grid is disabled.
- `block-cull` stores task bounds in a separate buffer and uses a spare pointer
  in the 64-bit point path. It is not a portable replacement for the batch path.
- `sample-lanes` tests one sample per invocation. Its strict certificate test
  prevents the sample lanes for a point from rejecting each other.
- `sample-generic` specializes the task loop for one or four samples with Slang
  value generics. This is the retained production change. `sample-count` and
  `sample-shared` test the same specialization with other source forms.
- `baseline-normalized` and `sample-normalized` strip debug instructions and
  compact SPIR-V IDs in the three raster compute entries. They test variation
  between equivalent shader binaries. They are not production build options.
- `lossless-points` stores two 16-bit float-bit offsets, one unchanged 32-bit
  coordinate, and the original ID when a task's bit ranges fit. Other tasks keep their original float values.
  It adds no quantization. The probe retains both layouts for comparisons.
- `lossless21-points` uses 21-, 21-, and 22-bit offsets for the three coordinates.
  It also preserves every float bit and falls back when a task does not fit.
  Test the two packing formats in separate campaigns. Their decoding and task
  metadata did not produce a useful speed gain on the large inputs.

The comparison workload uses a proxy scalar field, not a full detector result.
The line-sprite workload forms line segments from source triangle indices.
The probe does not model UI cost, display presentation, adaptive navigation,
cached frames, or asynchronous loading. Use the application benchmark for those
questions. See [the shader audit report](../../doc/shader-performance.md) for
measurements and decisions.
