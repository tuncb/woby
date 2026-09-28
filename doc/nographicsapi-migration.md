# NoGraphicsAPI renderer migration

Implemented and validated on Windows on 28 September 2026. NoGraphicsAPI is
the application's renderer; bgfx, bimg, shaderc, and the old shader sources
are no longer application dependencies. The `.woby` format remains version 16
and the application version remains 0.21.3.

## Implementation

`src/graphics.cpp` is a Woby-specific submission adapter over native
NoGraphicsAPI resources, pipelines, command buffers, and timeline semaphores.
It preserves the application's ordered views and draw submission conventions.
It is not a general implementation of the bgfx API. Scene logic, `UiState`,
operations, history, and serialization remain independent of graphics objects.

The adapter uses NoGraphicsAPIUtility's `BumpAllocator`, `TextureAllocator`,
`UploadQueue`, `DeleteQueue`, and shared shader types. Three frames in flight
retain draw resources through GPU completion. Geometry uploads use a staging
queue; transient roots, vertices, and indices use mapped frame arenas.
Texture updates publish new images and descriptor slots, so recorded draws
and in-flight font atlas readers keep their original contents. Destruction,
upload cancellation, resize, and asynchronous readback use the same timeline.

`shaders/native/woby.slang` contains all 23 shader entry points. The shared
`root.h` defines the C++/Slang data layout. The renderer covers:

- Mesh shading, transparency, transformed geometry, lines, and circular vertices.
- Comparison distance and quality heatmaps and diagnostic overlays.
- Grid, dimensions, surface annotations, and ImGui, including font atlas updates.
- Four-sample color/depth rendering and color resolves. Marker IDs remain
  separate, unblended, and unresolved; compute picking reads individual samples.
- Selection highlights, desktop presentation, headless rendering, and PNG export.

Circular vertex markers use the original renderer's pixel-rate cutout in both
ordinary drawing and the picking shader. Four-sample MSAA still handles geometry
and depth coverage; the circle itself is evaluated once per pixel. This restores
the earlier performance/appearance tradeoff, as requested after the
[rendering investigation](nographicsapi-renderer-investigation.md).

NoGraphicsAPI is pinned to `ae017a2f545abc0847e546cc7e84139bf3cc4241` with an
archive checksum. `cmake/PatchNoGraphicsAPI.cmake` extends that private dependency
copy with MSAA, line/strip topology, color resolves, and SDL Vulkan surface
callbacks. The Vulkan changes include sample-rate shading and variable surface
extents. The Metal changes include multisample texture/pipeline state, primitive
topology, and resolve attachments. Patch application is checked and idempotent.

The existing bx CPU math conventions are retained through an independently
pinned bx library. No bgfx rendering code is linked. Dependency notices are
staged in `assets/licenses`.

## Requirements and builds

Windows/Linux require Vulkan 1.4 and the full feature profile checked by
NoGraphicsAPI, including descriptor heaps, device-address commands, untyped
shader pointers, and mesh shaders. Vulkan version alone is insufficient.
The app reports initialization failure for unsupported hardware or drivers.
macOS requires Apple silicon, macOS 26+, and Metal 4.

Build tools: standalone Slang 2026.18.3 or newer, plus Vulkan SDK 1.4.357.0
and SPIRV-Tools 2026.3 or newer on Windows/Linux. macOS requires the Xcode 26
Metal toolchain. The CI bootstrap pins tool releases; CI continues to use
Ninja and vcpkg. Local Windows development uses the Visual Studio preset:

```powershell
. C:\tools\graphics-dev.ps1
cmake --preset vs2026-vcpkg -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2
```

The helper script above is this machine's toolchain setup, not a repository
dependency. On another machine put `slangc` and `spirv-val` on PATH, set
`VULKAN_SDK` and `VCPKG_ROOT`, or provide `WOBY_SLANGC`/`WOBY_SPIRV_VAL` to CMake.
`WOBY_TEST_HEADLESS` enables hardware-dependent tests; unsupported CI GPUs
should leave it off. The normal developer CTest preset excludes slow tests;
the explicit command above includes them.

Every Vulkan shader build runs `spirv-val`. Runtime assets contain the native
SPIR-V or Metal shader set, including the picking compute shaders. The package
manifest and updater now require these assets instead of Direct3D/OpenGL files.

## Validation on this machine

Windows, RTX 3070 Laptop GPU, NVIDIA 616.92, Vulkan device 1.4.351;
Visual Studio 2026 Debug build. Vulkan validation and synchronization validation
were enabled for GPU and viewer checks, with implicit layers disabled.

| Check | Result |
|---|---|
| Debug build | Passed without compiler warnings |
| Full CTest suite, including slow tests | 666/666 passed |
| CI helper tests | 19/19 passed |
| Native shader compilation | All 23 SPIR-V entries compiled and validated |
| Metal shader translation | All 23 entries translated by Slang; native Metal compilation/runtime not tested |
| Resource lifetime regression | Pixel assertions for old/new texture versions in one submission; destroy-before-submit and odd-sized index upload |
| Production GPU marker tests | Picking, occlusion, transparency, IDs, MSAA lookup/highlights |
| Fresh desktop startup | Four viewers, eight annotated PNG captures |
| Additional viewer workflows | Controls, history, lifecycle, comparisons, annotations, topology, holes, fins, degenerates, intersections passed |
| Manual desktop inspection | Camera orbit/zoom, maximize/restore at 3840x2160, menus, UI scale 100%/200%/100% |
| Stress scene | 2,500 distinct models: import, display changes, screenshots, save/new/reopen passed |
| Staged Debug package | Verified manifest, 23 native shaders and three license files; complete headless smoke passed from the staged executable |
| Binary dependency audit | Vulkan/SDL present; no bgfx or bimg dependency |
| Validation log scan | No Vulkan validation or synchronization errors in final viewer runs |

The stress run imported 2,500 models in 11.54 seconds. Its median measured
frame time was 154.75 ms in Debug with validation and the scene pane open.
These are functional stress observations, not a Release benchmark or a claim
of improved performance over bgfx.

Local logs and captures are under `build/migration-*` (ignored build artifacts).
The complete suite log is `build/migration-all-tests-final.log`; the stress
measurements are `build/migration-performance/measurements.json`.

Manual smoke scripts had stale version-15 and disabled-detector expectations.
They now assert the existing version-16 and manual-update behavior. The
comparison smoke also checks the existing annotation capacity error: two full
detector reports exceed the default 1800-pixel export height. It exports each
analysis separately and exercises both together in scene operations. No
production export capacity validation was relaxed.

## Release build follow-up

The first optimized build exposed a submission lifetime bug: `SubmitDesc`
borrowed initializer-list arrays that had already expired before submission.
Both the application and prototype now keep explicit arrays alive through
the submit call. An empty-frame GPU regression test covers startup and
command-pool reuse and is run in Release as well as Debug.

The corrected Visual Studio 2026 Release build is warning-free. All 667 Release
CTest tests passed, including slow and GPU tests. A fresh Debug build and the
three affected GPU regression tests also passed. The staged Release app passed
four desktop startups and eight annotated exports. Its ZIP was verified against
the package manifest and is available under `build/nographicsapi-release`.
Logs: `build/nographicsapi-release-build-fixed.log`,
`build/nographicsapi-release-tests-fixed.log`, and
`build/nographicsapi-release-startup-test.log`.

The [Release performance comparison](nographicsapi-performance.md) measures
five large OBJ models against the original main revision (`97886f5`). Edge
overlays improve by 15–20% on the four large models, while vertex overlays
regress by 26–34%. Solid rendering and the empty-scene control also identify
frame pacing as an investigation candidate. The report retains the matched
settings, GPU timings, and measurement limitations.

## Remaining platform and release checks

- Linux window presentation and native macOS builds/runs have not been tested
  on this Windows machine. Their backend and CI paths are implemented, but
  require those platforms for verification before publishing their packages.
- Older hardware covered by bgfx may not meet the new mandatory feature profile.
  There is no legacy rendering fallback in the application.
- Already-installed old updater binaries require the legacy shader inventory.
  They may reject the first native-only release before running its updated
  helper. Plan a bridge updater release or a manual first upgrade; do not claim
  seamless upgrades from old packages without testing that path.
- Allocation budgets are explicit: 16,384 texture descriptor slots, up to
  2 GiB of texture heaps, and 256 MiB of transient allocations per frame.
  Upstream NoGraphicsAPI still has assertion-based failure paths. GPU device
  loss and severe allocation exhaustion are not recovery-tested.
- Historical bgfx-specific programs under `tests/first_display`,
  `tests/hover_experiment`, and `tests/marker_experiment` are archived experiments,
  not maintained production test targets. Run them from a pre-migration revision.

The earlier [feasibility report](nographicsapi-feasibility.md) documents why the
upstream extensions were needed; its recommendation predates this implementation.
