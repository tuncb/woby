# One evaluation path for fixed freeform tessellation

Use the completed CPU mesh for rendering. First reuse each surface sample when computing its normal. Then remove the GPU evaluation that runs after upload. Keep native controls, knots, parameter samples, trimming, and source provenance.

This initial investigation uses production commit `cb63eb2`. The [implementation report](freeform-evaluation-implementation.md) records the subsequent change. The isolated [probe](../experiments/freeform-evaluation/main.cpp) originally compared the baseline evaluation loop with a sample reuse prototype and measured GPU packing. It now tests production sample reuse; packing has been removed. [Raw baseline results](freeform-evaluation-results.json) retain the original input, executable, and probe hashes. The current-path descriptions below refer to that baseline.

## Current work

1. OBJ and importer plugin loading call `appendFreeformGeometry`. It builds the fixed parameter grid, applies trim triangulation, and writes complete CPU positions, normals, UVs, triangle indices, and line indices. It also retains double source positions and native freeform data.
2. Each surface vertex calls `evaluateFreeform` for its position and UV. It then calls `freeformNormal`, which evaluates the same parameters again. Curves return the constant normal directly, so they do not have this second evaluation. Singular surface normals can require a further sample near the interior.
3. The normal upload path transfers the completed vertices and indices to GPU buffers.
4. The main loop checks `GpuMesh::freeformPrepared`. Once per GPU mesh lifetime, it calls `dispatchFreeformGpu`. This packs controls and basis tables on the main thread, creates one temporary input buffer per eligible patch, and dispatches a compute shader. The shader overwrites positions, valid normals, authored UVs, and untrimmed connectivity.

The grid uses 32 segments per active knot span. A degree-one curve uses one segment per span. The camera does not change the grid. The second evaluation adds no samples or refinement. A new GPU mesh can repeat this work after a reload or resource rebuild; ordinary camera motion does not repeat it.

Sources: [CPU construction](../src/freeform.cpp), [OBJ loading](../src/obj_mesh.cpp), [plugin loading](../src/importer_host.cpp), [upload](../src/scene_renderer.cpp), [main loop](../src/main.cpp), `src/freeform_gpu.cpp` at `cb63eb2`.

The GPU path is not independent. For a singular normal, the shader preserves the CPU normal already in the buffer. For a trimmed surface, it uses the CPU connectivity and does not write rectangular cells over the trim holes. Patches outside the float safety limits retain all CPU geometry. Packing still builds and checks their data before returning an empty result.

Sources: [compute shader](../shaders/native/woby.slang), `src/freeform_gpu.cpp` at `cb63eb2`.

## CPU geometry consumers

| Consumer | Existing CPU input | Effect of keeping one CPU result |
| --- | --- | --- |
| Bounds, framing, and dimensions | Mesh positions and double source positions | Keeps the required data and current behavior |
| CPU scene picking and surface annotations | CPU triangle positions, indices, and transforms | Removes the extra GPU evaluation as a cause of display disagreement |
| Analysis and comparison | Source positions, source identities, and tessellated faces | Keeps current analysis inputs |
| Surface, curve, edge, and point rendering | Uploaded vertices and indices | Uses the geometry already uploaded |
| Scene restoration | Reloaded source geometry and scene state | Uses the same fixed tessellation on reload |

This does not make all CPU and GPU coordinates bitwise equal. Rendering still uses float positions and GPU transforms; some CPU operations use doubles. It removes one additional source of differences.

Sources: [mesh positions and bounds](../src/model_mesh.cpp), [dimensions](../src/scene_dimensions.cpp), [picking](../src/scene_pick.cpp), [annotations](../src/surface_annotation.cpp), [analysis](../src/mesh_comparison.cpp), [freeform scene restoration checks](../tests/ctl_freeform_smoke.py).

## Measurements

Measurements ran on 10 October 2026, on an AMD Ryzen 7 5800H, with the Visual Studio 2026 x64 Release build from the `vs2026-vcpkg` preset. Models ran sequentially without concurrent builds or tests. Each row is the median of five warm runs. Evaluation pairs alternate order.

The evaluation loop calls production `evaluateFreeform` and either production `freeformNormal` or a probe copy of its normal rules that accepts the existing sample. Both loops consume positions, normals, and UVs in a checksum. These times exclude trim triangulation, mesh allocation and writes, parsing, upload, and GPU execution. They measure a possible CPU optimization, not a completed application change.

| Model | Freeform samples | Current evaluation loop | Reuse sample | Loop time reduction |
| --- | ---: | ---: | ---: | ---: |
| 1,000 synthetic bilinear panels | 1,089,000 | 701.7 ms | 389.5 ms | 44.5% |
| `tubes_as_nurbs.obj` | 137,127 | 101.4 ms | 55.4 ms | 45.3% |
| `SA_Row3_x01000.obj` | 2,234,000 | 1,995.4 ms | 1,102.2 ms | 44.8% |

The probe checked every normal separately for exact equality before timing. All matched. The keycap model exercised 129,000 singular fallback samples. The tubes model includes 695 curve samples; these do not save an evaluation. Probe contract checks also cover valid authored normals, zero authored normals, collapsed boundaries, and curves.

| Model | CPU time to pack GPU inputs | Eligible patches / total | Packed payload |
| --- | ---: | ---: | ---: |
| Synthetic panels | 14.1 ms | 1,000 / 1,000 | 4.98 MiB |
| Tubes | 36.3 ms | 55 / 63 | 2.97 MiB |
| 1,000 keycaps | 1,058.3 ms | 4,000 / 7,000 | 139.61 MiB |

Packing times include all patches, including those rejected for GPU evaluation. The payload is the sum of successful packed float arrays. It is not measured peak memory, allocation capacity, or retained mesh storage. The current successful dispatch path creates a GPU input buffer and issues one dispatch per eligible patch. These measurements exclude those allocations, copies, commands, and their GPU execution time.

For the keycap model, the isolated evaluation saving is about 893 ms. Removing GPU packing would also avoid about 1.06 seconds of main-thread CPU work in this workload. The current one-off CPU import took 18.91 seconds for context. Do not interpret the 44.8% loop reduction as a 44.8% load reduction: trimming and other preparation still run. The earlier [loading investigation](freeform-loading-performance.md) found trim triangulation to be a major cost.

BearTrap and BusGameMap were not used for these measurements. The native freeform models exercise the work under investigation directly.

## Benefits and costs

| Change | Benefit | Cost or limit |
| --- | --- | --- |
| Reuse the CPU sample for normal generation | Removes one complete surface evaluation per vertex; exact normal agreement in tested cases | Must preserve authored-normal precedence and the exceptional interior sample |
| Render the completed CPU geometry | Removes duplicate GPU evaluation and main-thread packing; avoids transient control and basis uploads | Pixels can change because the current compute result uses different float arithmetic |
| Remove compute-specific integration | Removes the readiness flag, fallback selection, shader program lifecycle, packing code, and duplicate evaluator | Requires shader registry and graphics adapter cleanup as well as application cleanup |
| Keep source controls and grids | Preserves provenance, deterministic tessellation, and future reevaluation | Retained CPU source, grid, mesh, and GPU render storage largely stay the same |

The expected benefit is loading and the first frame after GPU mesh creation. No steady frame-rate improvement is established: the existing dispatch already runs once. This change does not accelerate polygon-only models, reduce tessellation density, or remove trim triangulation.

CPU evaluation uses double intermediates and local references. GPU evaluation uses packed floats, its own derivative scaling, and different normal validity tests. Existing equivalence tests allow a relative tolerance of `2e-5`; they do not establish identical pixels. Removing compute can change lighting or boundary pixels. Inspect rational surfaces, authored normals, singular boundaries, large coordinates, and trimmed holes before accepting the change.

Sources: [CPU normal rules](../src/freeform.cpp), [shader normal rules](../shaders/native/woby.slang), [GPU equivalence test](../tests/freeform_gpu_tests.cpp).

## Alternative with GPU evaluation first

A GPU-first design could become useful for adaptive tessellation or frequently edited control nets. Neither is present in this fixed path. Today it would need another design for CPU bounds, picking, annotations, analysis, and singular normals. CPU trim triangulation would still be required unless it was also replaced. Readback adds synchronization; deferred CPU evaluation adds cache and publication rules. Those changes are much larger than removing the duplicated work.

A CPU basis cache is another possible optimization because regular grids repeat U and V parameters. It is independent of this proposal. First remove the known duplicate sample call; measure the remaining evaluator before adding cache storage or new indexing.

## Recommended implementation order

1. Add a normal helper that accepts a `FreeformSample`. Use it from `appendFreeformGeometry`. Keep the existing parameter-based wrapper where callers need it. Preserve curve normals, authored normals, singular fallback sampling, and progress/cancellation checks.
2. Remove first-use GPU dispatch from the main loop and use the uploaded CPU buffers directly. Compare before/after viewer captures, CPU picks, annotation placement, and geometry counts. Include mixed polygons and curves, rational and trimmed surfaces, scene reload, rebase, and large-coordinate cases.
3. Remove `freeform_gpu.cpp/.h`, the compute shader entry, program creation/destruction, readiness state, and obsolete packing/equivalence tests. Update the shader build list and asset registry. Remove only the unused graphics binding and root data. If the root layout changes, update its offsets and rebuild all shader entries. Keep general compute buffers and bindings used by picking and other rendering.
4. Run warning-free Debug build and unit tests, then freeform rendering checks and full viewer load/first-frame measurements. Keep the same models and source hashes for comparison. Record CPU packing removal separately from GPU timing. Commit after those checks pass.

The native freeform metadata and the point-hierarchy exclusion for meshes with `mesh.freeform` can stay unchanged in this work. Removing either needs a separate consumer review.

Integration sources: [shader build list](../cmake/NoGraphicsAPI.cmake), [asset registry](../src/graphics_assets.h), [graphics adapter](../src/graphics.cpp), [shader root layout](../shaders/native/root.h), [mesh preparation](../src/scene_mesh_preparation.cpp).

## Reproduction and validation

Enable `WOBY_BUILD_BENCHMARKS`, then run:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_freeform_evaluation_probe
./experiments/freeform-evaluation/run.ps1
```

The runner accepts `-Models`, `-Executable`, and `-Output`. The baseline version recorded five evaluation pairs and five packing runs per model, plus hashes. The current version records the evaluation pairs only. It does not require a GPU device. Use Release results for timing; the Debug probe supplies the contract check.

The full Debug build and Release probe build completed without warnings. All 78 Debug unit test groups passed, including the new probe contract and the existing freeform CPU, trim, and GPU equivalence tests. These checks preceded the production change; subsequent validation is in the implementation report.
