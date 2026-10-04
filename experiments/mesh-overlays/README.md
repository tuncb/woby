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

Pass timestamps are within one render pass; the final marker interval includes
color resolve/end-pass work. Legacy draws are interleaved, so only their total
GPU time is reported. CPU submission excludes fence waits. The harness completes
each frame before reusing resources: these are scene GPU timings, not viewer FPS
or end-to-end interaction latency. ID mode measures the extra production-style
attachment, not the full hover lookup/highlight/UI workflow.

All methods retain the same original geometry, marker IDs, and control edge
buffer in a process. This isolates drawing costs from residency changes. The
native path does not use that edge buffer for visible surface edges; its possible
memory saving is reported as a calculated payload, not a measured VRAM reduction.
No geometry decimation, marker sampling, or visibility culling is implemented.
Model colors are deterministic per group; global opacity is the transparency
control. Arbitrary saved scenes, imported lines, detectors, and interleaved opaque
and transparent groups remain production-integration work.
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

References: [issue 106](https://github.com/tuncb/woby/issues/106),
[Khronos barycentric sample](https://docs.vulkan.org/samples/latest/samples/extensions/fragment_shader_barycentric/README.html).
