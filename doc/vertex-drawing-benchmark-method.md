# Vertex drawing experiment (archived)

The experiment source was removed when the shader path became the production
renderer. These instructions describe commit `0cb4419`; use that revision to
reproduce the historical benchmark. No expanded-geometry fallback remains in
the application.

This opt-in executable compares production expanded markers (`current`),
four shader-generated vertices per marker instance (`shader`), and six generated
vertices per marker without instancing (`flat`). It does not change viewer
behavior or its saved scenes. The two experiments upload the production compact
point-ID list and read XYZ from the existing 32-byte mesh vertex buffer.

The measured findings and limitations are in
[performance results](vertex-drawing-performance.md).

Build and validate on Windows:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_VERTEX_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure
cmake --build --preset vs2026-vcpkg --config Release --target woby_vertex_benchmark
uv run tests/vertex_drawing/contract_test.py D:/woby/build/vs2026-vcpkg/bin/Release/woby_vertex_benchmark.exe
uv run tests/vertex_drawing/run.py --binary D:/woby/build/vs2026-vcpkg/bin/Release/woby_vertex_benchmark.exe --models D:/temp/obj_tests --output D:/woby/build/vertex-drawing-results-new --rounds 3
uv run tests/vertex_drawing/summarize.py build/vertex-drawing-results-new/runs.jsonl build/vertex-drawing-results-new/summary.json
```

Use a fresh output directory. The driver runs fresh processes serially, rotates
variant order each round, and writes successful observations and failures to
`runs.jsonl`. No build, test suite, or other benchmark should run during timing.
The harness requests NVIDIA hardware, Direct3D 11 on Windows (Vulkan elsewhere);
only the Windows path has been runtime-validated. All model/output paths are
absolute. Contract fixtures and their captures live in one temporary directory
and are cleaned even on failure.

`generate.py` extracts the actual production buffer construction and draw
functions from `scene_renderer.cpp`. It changes only the main vertex buffer's
creation flags to permit shader reads for experimental variants. It fails if its
source anchors change. The production OBJ loader is linked directly. Both shader
variants use the original circle fragment shader. The flat version reproduces
the original triangle order; the instanced strip uses the same quad diagonal.

Each model uses all groups, a fixed oblique camera fitted to its bounding sphere,
and a 1280x720 RGBA8/D24S8 offscreen target without MSAA or VSync. This isolates
geometry drawing and resembles the screenshot path, not the entire windowed UI.
Per-group colors are deterministic. The measured scenarios are 4-pixel opaque
points, opaque solid mesh plus 4-pixel points, and 8-pixel points at opacity 0.4.
Solid surfaces are drawn before the point pass. CPU picking, UI, annotations,
camera motion and presentation are excluded.

Timing fields are milliseconds:

- `load_ms`: production CPU OBJ load, reported separately.
- `base_cpu_ms` / `base_ready_ms`: membership creation and main mesh upload;
  excluded from marker-enable costs.
- `enable_cpu_ms`: marker CPU preparation plus enqueueing its buffers.
- `enable_ready_ms`: the same interval through a submitted frame and GPU
  readback completion. It includes driver allocation/upload processing and the
  fence overhead.
- `first_draw_ready_ms`: enable through the first opaque point draw and readback
  completion, forcing actual resource use as well as allocation.
- `gpu_markers`: bgfx GPU timestamps for the marker view.
- `gpu_frame`: GPU timestamps for the full submitted scene frame, including
  the solid pass when enabled.
- `cpu_submit`: application-side scene command generation, excluding `frame()`.
- `wall_frame`: submission plus `frame()` waiting; asynchronous pipeline timing,
  not a standalone GPU execution measurement.
- `completed_mean_frame_ms`: entire measurement interval, including final GPU
  completion, divided by its submitted scene-frame count.

Each steady scenario warms for at least one second and 12 frames, then measures
at least two seconds and 30 frames. GPU results are asynchronous: only distinct
GPU frame IDs within the measurement window are included. Startup/warmup samples
and readback frames are excluded from GPU distributions. Readbacks occur outside
the timed drawing loop. Per-frame samples are summarized in each process;
`summarize.py` reports medians of the independent run medians, their range, and
sample counts. It rejects changed workloads or missing timings, and does not
infer a speedup from an unmatched successful variant.

`marker_payload_bytes` is exact application buffer payload (104P or 4P), not
measured VRAM residency. Windows process counters include loader/driver/storage
effects; peak counters are cumulative. bgfx's NVIDIA memory counter reflects
adapter usage and can include other applications, so it is not a reliable
per-process VRAM measurement. Memory figures and timings need separate
interpretation.

The contract test checks ordered group membership and pixel-identical output on
shared IDs, normal seams, coincident markers, repeated corners, occlusion and an
unused source vertex. It includes perspective/orthographic views, transparency,
and 1/4/8/40-pixel markers. The sweep also saves raw RGBA frames and reports pixel
differences for every successful real-model comparison. Numerical equality is
checked before drawing-performance conclusions are made. Six analysis unit tests
cover weighting, failed/unpaired runs, mismatched membership, missing GPU samples
and selection of the non-instanced results.
