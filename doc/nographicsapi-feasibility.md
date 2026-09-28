# NoGraphicsAPI migration feasibility

Historical assessment: the migration described in
[renderer architecture and validation](nographicsapi-migration.md) now replaces
bgfx in the application. The recommendations below predate that implementation.

Researched 28 September 2026. Woby revision: `97886f5fa86f09256b95237016fbbd3315670a73`.
NoGraphicsAPI revision: [`ae017a2f545abc0847e546cc7e84139bf3cc4241`](https://github.com/sebbbi/NoGraphicsAPI/commit/ae017a2f545abc0847e546cc7e84139bf3cc4241).
Findings below concern that source snapshot, not a promise about future versions.

**Recommendation: prototype a selectable NoGraphicsAPI renderer, using its utility library, while keeping bgfx as the production default.** A migration is technically plausible for modern Windows and Apple silicon Macs. An immediate replacement preserving all current platforms, hardware coverage, and rendering behavior is not supported by the present library. The largest gaps are MSAA, primitive topology, Linux presentation, and resource-failure handling. No performance improvement has been measured.

This assessment assumes woby should preserve its current behavior and Windows/Linux/macOS releases. Restricting the product to newer Windows/Mac systems materially reduces the project, but accepting that restriction is a product decision.

**What the library provides.** This is a C++20 library with native Vulkan and Metal 4 implementations, shared Slang shaders, address-based mesh data, and indexed texture/sampler heaps. It is not Vulkan-only and does not use MoltenVK. Both the core and utility library use the MIT license. Woby already uses C++20. See the [upstream overview](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/README.md) and [license](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/LICENSE).

**The current woby migration surface.** Direct `bgfx::` or `BGFX_` references occur in 16 files under `src`; CMake compiles 23 shader entry points. These counts locate coupling, not the number of files that would ultimately change.

| Area | Current implementation | Migration work |
|---|---|---|
| Initialization and frame loop | `main.cpp`, `renderer_startup.cpp`, `bgfx_helpers.*` | Device creation, SDL native handles, frame pools/timelines, presentation, capability messages, resize, statistics and shutdown. |
| Meshes, edges, vertices and helpers | `scene_renderer.*` | Replace buffer handles/layouts/uniforms with heap ranges and shared root records; preserve ordering, transforms, opacity, optional uploads and marker IDs. |
| Analysis display | `comparison_view.*` | Port heatmaps, quality surfaces, diagnostic lines and highlighted triangles. CPU analysis algorithms remain reusable. |
| Vertex picking | `marker_pick.*`, `shaders/marker/*` | Port color/ID attachments, per-sample lookup, highlight/composite passes, asynchronous readback and stale-result rejection. |
| Surface annotations | `annotation_ui.*` | Port transient stroke drawing. Existing surface geometry and editing logic can remain. Strokes are already expanded into triangles. |
| ImGui | `imgui_bgfx.*` | Write a renderer adapter, including dynamic texture create/update/destroy, clipping, index offsets and offscreen text drawing. Keep the SDL3 input backend. |
| PNG export | `scene_screenshot.*` | Offscreen attachments, scene/legend rendering, texture-to-memory copies and completion tracking. Keep PNG encoding and export options. |
| Build and tests | `CMakeLists.txt`, `cmake/BgfxShaders.cmake`, workflow, GPU fixtures | Add Slang/SPIR-V/metallib compilation and packaging; port renderer fixtures and image checks. |

The `UiState`/`ui_operations`/scene-file boundary is a useful foundation. A renderer migration does not itself require a `.woby` format change. Keep GPU objects, allocators, descriptors and timelines in runtime structs. Preserve deterministic CPU geometry and picking functions. There is additional `bx` math usage in camera, transforms, picking and annotations; deleting bgfx does not automatically eliminate that dependency.

**Compatibility and feature gaps.**

| Requirement | Finding | Consequence for woby |
|---|---|---|
| Modern Windows graphics | Vulkan 1.4 plus descriptor heaps, device-address commands, shader untyped pointers and task/mesh features are required. | Existing D3D11/GL-era coverage contracts substantially. Vulkan 1.4 alone is not a sufficient check. |
| macOS | Native Metal 4; macOS 26+ and Apple GPU family 7+. Upstream M1/M2 execution remains unverified. | Fits the current ARM64 release architecture, but raises the OS baseline and needs actual M1/M2 testing. |
| Linux desktop | Vulkan library/headless support exists; Win32 is the only Vulkan presentation backend. | Need upstream/fork work for X11/Wayland presentation, or retain bgfx on Linux. SDL alone does not supply the missing backend integration. |
| 4x MSAA | No sample-count or resolve interface; Vulkan texture creation and rasterization hard-code one sample. | Current viewport and per-sample marker picking cannot be ported faithfully without extending the library. |
| Lines and strips | Conventional raster draws hard-code triangle lists in Vulkan and triangles in Metal. | Add primitive-topology support or replace edge/helper lines with triangle strokes. Convert four-vertex marker/fullscreen strips to six vertices or indexed triangles. |
| Failure recovery | Vulkan allocation paths use abort-on-failure helpers. | An exhausted native allocation may terminate the process; this needs attention for a viewer loading arbitrary large meshes. |

Platform evidence: [Vulkan implementation](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/vulkan-support.md), [build targets](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/building.md), and [Metal validation](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/metal-validation.md).
Feature evidence: [public descriptions and commands](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/include/NoGraphicsAPI/NoGraphicsAPI.hpp#L477-L665), [single-sample Vulkan textures](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/src/NoGraphicsAPI.cpp#L2491), [Vulkan topology and sampling](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/src/NoGraphicsAPI.cpp#L2835-L2858), [Metal draw topology](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/src/NoGraphicsAPIMetal.mm#L1765-L1807), and [Vulkan allocation handling](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/src/NoGraphicsAPI.cpp#L720-L753).

For MSAA parity, an extension needs multisampled textures, compatible pipeline state, resolve attachments, format/sample-count queries, and multisample shader views on both backends. Woby must resolve visible color while retaining unresolved ID samples for picking. Replacing this with single-sample rendering or post-process antialiasing changes coverage and selection behavior; neither is a transparent substitute.

Basic indexed meshes, instancing, independent attachment blending, depth tests, compute, storage textures, and texture readback have corresponding core operations. This supports the feasibility of the main port, but does not establish correctness or format support on every device. Query attachment formats rather than assuming woby's current D24S8 choice works everywhere.

On Windows, woby already obtains an SDL `HWND`, matching `DeviceDesc::window`. On Mac, its current Cocoa-window handoff must become a `CAMetalLayer` handoff. SDL provides [SDL_Metal_CreateView](https://wiki.libsdl.org/SDL3/SDL_Metal_CreateView) and [SDL_Metal_GetLayer](https://wiki.libsdl.org/SDL3/SDL_Metal_GetLayer); the adapter must manage layer/view lifetime, drawable sizing and main-thread operations.

**Use the utility library selectively.** It addresses substantial infrastructure work and is worth adopting with the renderer. It does not implement a scene renderer, SDL integration, ImGui adapter, render graph, or the missing MSAA/topology features.

| Utility | Proposed use | Policy woby must still supply |
|---|---|---|
| `BumpAllocator` | Per-frame root records, ImGui geometry and temporary overlay vertices. | Frame storage capacity and retirement. It is fixed storage, not an automatically growing ring. |
| `HeapAllocator` | Long-lived mesh vertices, indices and analysis data in a small number of large heaps. | Heap sizing/growth, fragmentation handling, allocation failure and delayed reuse. |
| `TextureAllocator` | Offscreen color/depth/ID targets and UI textures. | Texture-heap budgets, descriptor indices, view lifetime and resize retirement. |
| `UploadQueue` and texture upload helpers | Bounded staging for model data and font/texture updates. | Upload scheduling, backpressure, destination lifetime and cross-queue waits. |
| `DeleteQueue` | Retire removed meshes, old targets and other GPU objects safely. | Correct completion values, ordered entries, bounded capacity and shutdown draining. |
| Shared shader types | Explicit CPU/Slang layouts and pointer fields. | Size/offset checks and matrix conversion at the rendering boundary. |
| Math | Optional renderer-local calculations. | AVX2/FMA baseline on x86 and deliberate conversion from existing bx conventions. |

These responsibilities follow the [bump allocator](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/utility/include/NoGraphicsAPIUtility/bump_allocator.hpp), [heap allocator](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/utility/include/NoGraphicsAPIUtility/heap_allocator.hpp), [texture allocator](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/utility/include/NoGraphicsAPIUtility/texture_allocator.hpp), [upload queue](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/utility/include/NoGraphicsAPIUtility/upload_queue.hpp), and [deletion queue](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/utility/include/NoGraphicsAPIUtility/delete_queue.hpp) contracts.

Link `types`, `allocators`, `textures` and `uploads` initially; avoid the aggregate `utility` target if retaining bx math. The aggregate pulls in `math`, whose x86 compile options enable AVX2/FMA. The GPU utilities depend on NoGraphicsAPI; they are not a drop-in bgfx resource manager. See the [component targets](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/utility/CMakeLists.txt).

The core API fits procedural runtime code well. Several upstream utilities are classes with private members. Using them as third-party implementation details is consistent with the request to use the utility library; woby's own adapters should remain structs and free functions, without inheriting from the utilities or exposing them through logical UI state.

Keep large static models in GPU-only heaps with a bounded mapped staging area. Placing every mesh in mapped memory would unnecessarily depend on BAR capacity. Upstream Metal also has a 64-live-GPU-heap address index; suballocate across a few heaps rather than creating one heap per group. See [Metal heap handling](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/metal-support.md). Utility suballocation exhaustion can return an empty result, but that does not make native Vulkan heap allocation recoverable.

**Architecture and performance implications.** Start with the existing rendering algorithms. Use ordinary indexed vertex shaders; meshlets are not required to display woby's meshes. A shared draw record can carry vertex/point-ID pointers, transform, color, point size and marker base ID. Shared integer fields remove the need to encode some IDs into float uniforms. The [shader contract](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/slang.md) uses C-compatible layout and row-major matrices: do not copy bx matrices blindly and assume the same interpretation.

Keep a small procedural backend boundary for mesh lifetime, frame submission, picking, ImGui and capture. A build-time backend option is sufficient for the first comparison. Avoid duplicating scene rules or building a general graphics framework. Preserve lazy edge/point uploads and transparent draw order.

Explicit lifetime and synchronization replace work bgfx currently performs. Track frame completion before reusing root records, resetting command pools, recycling descriptor slots, or freeing heaps. Record the actual dependencies between attachment writes, picking compute, highlight sampling and host readback. Start on one general queue; introduce asynchronous queues only after correctness and timing evidence justify them. The [core submission contract](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/include/NoGraphicsAPI/NoGraphicsAPI.hpp#L675-L803) makes these application responsibilities.

Potential gains are simpler binding code, control over uploads, and lower submission overhead in scenes with many groups. Fewer API calls do not establish faster frames. Woby's [large-model measurements](large-model-rendering-performance.md) show GPU-heavy edge/marker workloads and a historical CPU-hover bottleneck. Those measurements use an older revision; current woby already contains GPU marker picking. An API change does not automatically reduce triangle count, marker overdraw, CPU analysis or model parsing.

Compare the current bgfx baseline and the prototype on identical models, views, resolution, antialiasing and visible features. Measure CPU recording, GPU time, P50/P95 interaction latency, first display, upload stalls and peak memory. Include both many-group scenes and very large single-group meshes. A 1x-MSAA prototype must not be presented as a performance win over 4x-MSAA bgfx.

**Build, maintenance and validation.** Pin the reviewed upstream commit or a reviewed fork. Integrate through CMake source/package targets and keep vcpkg for the remaining dependencies. Preserve local `vs2026-vcpkg` and CI Ninja + vcpkg workflows. Replace shaderc outputs with build-time Slang outputs: SPIR-V on Vulkan and metallib on Apple. The upstream build currently requires Vulkan SDK 1.4.357+, Slang 2026.14.1+ for Vulkan, SPIRV-Tools 2026.3+, and Slang 2026.18.2+/Xcode 26+ for Apple shader builds. Package compiled shaders rather than adding a runtime compiler dependency. See [building and integration](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/building.md).

Upstream publishes useful tests and limitations, but its Metal validation report records 29/30 tests passing on M3 Max, with an outstanding render-timestamp failure. The [driver issue log](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/docs/known-driver-issues.md) also records readback and multi-queue failures on NVIDIA 596.99. These are specific observations, not evidence that every current driver fails. They make readback and queue testing essential for woby. Treat upstream changes as reviewed dependency updates, especially while the root ABI is evolving.

Woby's `scene_renderer_tests.cpp` uses bgfx's Noop renderer; headless NoGraphicsAPI still needs supported hardware. Keep CPU contract tests runnable without a GPU. Rework the small Noop-dependent fixtures and run real rendering tests on capable Windows and Mac machines. A hosted CI runner or software Vulkan implementation must not be assumed to expose the new extensions. Preserve existing marker, annotation, capture and CLI smoke coverage, including resize/removal while readbacks are pending.

**Local verification (updated after driver and development-tool setup).** Checks on this workstation found:

| Item | Installed/reported | Assessment |
|---|---|---|
| GPU | NVIDIA GeForce RTX 3070 Laptop GPU | Required queried features and device-local, host-visible coherent memory are present. |
| Driver/device Vulkan | NVIDIA 616.92; device Vulkan 1.4.351, loader 1.4.341 | Meets the Vulkan 1.4 version requirement. Previously NVIDIA 546.30 / Vulkan 1.3.260. |
| Required extensions | Descriptor heap, device-address commands, shader untyped pointers and mesh shader all present | The missing runtime extensions are resolved. Presentation maintenance extensions and optional unified image layouts are also present. |
| SDK and shader tools | SDK 1.4.357.0, standalone Slang 2026.18.3, SPIRV-Tools 2026.3 | Meets the build requirements. The SDK's bundled Slang 2026.13.1 is too old; select the standalone installation. |

Commands used were `vulkaninfo --summary`, full extension/feature enumeration through `vulkaninfo`, `slangc -version`, `spirv-val --version`, and Windows video-controller queries. The installed `vulkaninfo` does not decode the device-address-command feature structure, so a direct `vkGetPhysicalDeviceFeatures2` query using the Khronos header definition additionally confirmed `deviceAddressCommands = true`. All mandatory feature flags in the reviewed backend's device-selection check are supported, including BC compression; ASTC is not required when BC is available. This is a capability check, not a rendered-example test.

The user upgraded the NVIDIA driver and installed Vulkan SDK 1.4.357.0 between checks. A follow-up installed [Slang 2026.18.3](https://github.com/shader-slang/slang/releases/tag/v2026.18.3) at `C:\tools\slang\2026.18.3`, verifying the official archive SHA-256. SPIRV-Tools 2026.3 is supplied by the new SDK. The persistent user `CMAKE_PROGRAM_PATH` selects these versions; `. C:\tools\graphics-dev.ps1` activates the same tools in an existing PowerShell terminal. This activation matters because the SDK's bundled `slangc` otherwise precedes the user installation in the system PATH.

NoGraphicsAPI was built separately under `D:\.worktree\nographicsapi-research-2b09d5e2\build-dev-check`, using Visual Studio 2026 in Debug. The cube example and its vertex/fragment shaders built and passed SPIR-V validation without reported warnings. The upload-queue compute shader also compiled and validated. The selected upstream `test_api`, `test_upload_queue`, and `test_upload_queue_families` tests all passed (3/3) on this machine. This verifies device creation and real GPU upload/compute paths, not a full rendering or compatibility suite. Windowed presentation remains untested. No woby migration implementation, woby build, woby unit-test run or comparative benchmark was performed.

**Suggested implementation sequence and decision gates.**

1. Establish a compatible development machine and verify upstream examples/tests. Define the supported OS/GPU matrix and whether bgfx remains available for older devices/Linux.
2. Build a small selectable Windows prototype using utility allocators/uploads: SDL window, one real woby model, transforms, opacity, markers, ImGui and PNG readback. This establishes integration viability, not parity.
3. Resolve the library gaps before broad rollout: implement/test MSAA and primitive topology on both native backends, or explicitly approve alternate rendering behavior. Address allocation-failure recovery. Evaluate Linux WSI work separately.
4. Port the remaining analyses, annotations, GPU picking and export paths. Test overlapping markers and per-sample IDs, transparent occlusion, resize/minimize, cancellation, undo/removal with in-flight work, UI scale/font updates, and large exports. Compare output and timing with bgfx.
5. Validate on modern NVIDIA and AMD Windows systems and Apple silicon, including M1/M2. Retire bgfx only after supported-platform behavior and the measured benefits justify the maintenance change.

Planning estimate, not a delivery commitment: allow roughly **3-5 engineering days for a restricted Windows prototype after toolchain/driver readiness**, and **6-12 engineering weeks for a production migration with comparable Windows/Mac behavior, library extensions, Linux presentation and hardening**, assuming one experienced graphics engineer and suitable test machines. Upstream acceptance, driver problems and hardware availability can extend that range. Keeping bgfx for Linux/older devices reduces initial scope but creates ongoing dual-renderer maintenance.

The highest-value next experiment is an end-to-end marker-picking and screenshot slice with representative large meshes. Successful rendering of a triangle alone would leave the main feasibility questions unanswered.
