# Fixed freeform tessellation implementation

Fixed freeform geometry now has one CPU evaluation path. Rendering uploads the completed CPU vertices and connectivity. It no longer evaluates the same control net again on the GPU.

## Changes

`appendFreeformGeometry` passes its existing `FreeformSample` to a normal helper. The helper preserves authored-normal precedence, the interior sample used at singular boundaries, and the final default normal. The parameter-based wrapper remains available for callers that need a normal without an existing sample. Curves retain their constant normal.

The freeform GPU packer, dispatch function, compute shader, program lifecycle, and readiness flag are removed. The graphics adapter no longer accepts the unused freeform storage binding. Its shared shader structure is eight bytes smaller; field offsets and layout tests were updated. All shader entries were rebuilt. General buffers and compute paths used by other rendering features remain available.

The spline basis implementation is now internal to the CPU evaluator. Native controls, knots, grids, trim loops, source IDs, double source positions, tessellation density, and cancellation points remain unchanged. OBJ and importer plugins use the same path. No scene format or importer ABI change is required.

Sources: [CPU evaluation](../src/freeform.cpp), [normal helper contract](../src/freeform.h), [graphics adapter](../src/graphics.cpp), [shader layout](../shaders/native/root.h).

## Validation

- The full Debug build and Release viewer/probe builds passed without compiler warnings with the `vs2026-vcpkg` preset.
- All **78 Debug unit test groups passed**. The new normal tests cover authored normals, zero authored normals, a singular edge whose correct normal differs from the default, fully collapsed surfaces, extreme rational weights, and large rebased coordinates.
- The freeform GPU test now uses the production mesh upload and requires exact readback equality for positions, normals, UVs, triangles, and lines. It covers rational surfaces and curves, mixed primitive offsets, authored normals, trimmed holes, and rebasing near coordinates of magnitude `1e12`.
- All **three viewer checks passed**: `woby_headless`, `woby_freeform_render`, and `woby_coordinate_origin`. The freeform check includes annotation drawing and exact scene-reload capture agreement, as well as curves, trim holes, and undo.
- Existing CPU picking checks still select geometry behind a trim hole and select the freeform surface outside the hole.

Sources: [normal tests](../tests/freeform_tests.cpp), [exact upload test](../tests/freeform_gpu_tests.cpp), [picking and trimming](../tests/freeform_trim_tests.cpp), [viewer checks](../tests/ctl_freeform_smoke.py).

The local GPU validation uses NoGraphicsAPI Vulkan on Windows. Metal was not run on this machine.

## Measurement method

The baseline viewer was built from production commit `cb63eb2` before the implementation. Its executable, DLLs, and shader assets were saved together. Both viewers use Visual Studio 2026 x64 Release builds. The [viewer runner](../experiments/freeform-evaluation/viewer.py) starts a fresh headless process for each run, disables grid/origin/dimensions, then measures `model.add` through completion of the first screenshot. This includes RPC overhead, deferred first-frame work, GPU readback, and PNG encoding. It excludes viewer startup and is not an isolated GPU timing or FPS test.

There are three runs per build and model, with alternating build order. Builds and tests did not run during the measurements. File caches were not cleared. Geometry counts, bounds, origins, camera settings, and captures are compared between builds.

## Results

The local machine uses an AMD Ryzen 7 5800H. The table shows median wall times from the Release viewers. [Raw results](freeform-evaluation-implementation-results.json) contain all samples, executable and model hashes, CPU probe results, geometry statistics, camera settings, and pixel differences.

| Model | Before | After | Change in time |
| --- | ---: | ---: | ---: |
| Trimmed sheet, 1,652 vertices | 0.694 s | 0.720 s | 3.8% more |
| Tubes, 137,171 vertices | 1.625 s | 1.557 s | 4.2% less |
| 1,000 keycaps, 2,234,000 vertices | 25.643 s | 20.207 s | **21.2% less** |

All three keycap pairs improved. Baseline times ranged from 24.934 to 25.986 seconds; new times ranged from 20.127 to 21.134 seconds. The keycap model retains 2,995,000 triangles and 7,000 groups. The improvement includes both CPU sample reuse and removal of GPU preparation and evaluation; these application measurements do not separate those contributions.

The small sheet was about 26 ms slower at the median. Its screenshot stage alone took about 0.59 seconds. These three runs do not establish a small-model speedup. The change targets repeated evaluation and first-frame preparation, and does not establish a steady FPS improvement.

The production CPU probe also checked all 3,460,127 freeform samples across its three workloads against the retained normal reference rules. All normals matched exactly, including 129,000 singular fallback samples in the keycap model. This is separate from the viewer timing and image comparison.

The baseline investigation measured 139.6 MiB of successful packed inputs and about 1.06 seconds of CPU packing for the keycap model. That entire preparation path is now absent. This removes transient input data and dispatch work; it does not remove retained spline metadata or the CPU/GPU render meshes.

## Capture comparison

All six runs per model used matching geometry counts, bounds, origins, render modes, and camera settings. Each capture is 1,920 × 1,800 pixels.

| Model | Pixels changed between builds | Repeat behavior |
| --- | ---: | --- |
| Trimmed sheet | 1 of 3,456,000 | Same difference in all three pairs; one green-channel level |
| Tubes | 169 of 3,456,000 | Same difference in all three pairs |
| 1,000 keycaps | 0 of 3,456,000 | Pixel-identical in all three pairs |

The sheet and tubes captures were also inspected. The trim hole remains open and the tubes render without an apparent shape change. The small differences are consistent with replacing float GPU evaluation with uploaded CPU results. They do not imply pixel identity for every model or graphics device. Capture files and difference images remain under `build/freeform-evaluation/viewer`.

## Reproduction

Save the baseline executable, DLLs, and shader assets in a separate folder before building the new viewer. Then run the paired experiment after all builds and tests finish:

```powershell
uv run experiments/freeform-evaluation/viewer.py `
  --before D:/woby/build/freeform-evaluation/before/woby.exe `
  --after D:/woby/build/vs2026-vcpkg/bin/Release/woby.exe `
  --output D:/woby/build/freeform-evaluation/viewer `
  D:/woby/assets/samples/freeform-trimmed.obj `
  D:/temp/obj_tests/parametric/tubes_as_nurbs.obj `
  D:/temp/obj_tests/parametric/SA_Row3_x01000.obj
```

Run `experiments/freeform-evaluation/run.ps1` separately for the CPU comparison. It uses the normal-only wrapper as the repeated-evaluation reference and the new production helper as the reuse path. It no longer measures GPU packing. The [initial investigation](freeform-evaluation-investigation.md) retains the baseline packing measurements.
