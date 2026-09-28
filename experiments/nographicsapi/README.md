# NoGraphicsAPI migration prototype

An opt-in Windows x64 executable that exercises the renderer migration risks using
woby's `UiState`, `Mesh`, `Vertex`, marker ID allocator and readback acceptance
logic. It does not change the production bgfx renderer or `.woby` format.

The five fixed scenes deliberately contain overlapping vertices, IDs above 2^24,
transparent/hidden geometry and an occluding surface. Coordinates are in clip
space so selection and image assertions have deterministic expectations. The UI
selects test scenarios; it does not introduce new editable scene properties.

## What this tests

| Risk | Implementation and acceptance check |
| --- | --- |
| Exact picking with transparency | RGBA8 color plus **R32_UINT** ID MRT, independent blend states, IDs 16777217 and 33554435. Hidden/zero-opacity markers and a translucent occluding surface have explicit expected results. |
| Native antialiasing | 1× and 4× MSAA color/depth/ID attachments. Resolve color only; a 7×7 radius-3 compute lookup reads every integer ID sample. Per-sample circle interpolation preserves marker edge coverage. The 4× boundary test requires mixed ID samples. |
| Immediate hover feedback | GPU selection is consumed in the same submission by a highlight pass, before the CPU processes the asynchronous 16-byte result. |
| Mesh edges | Native line-list PSO, indexed directly from the fixture's triangle connectivity; readback verifies a line pixel at both sample counts. |
| Scene replacement and lifetime | Three independent frame arenas, command pools, targets and descriptor regions. Scene epochs reject old picks. Twenty-four replacements vary render size and sample count; deferred deletion retires the old GPU mesh after its last frame. |
| Bounded upload memory | Utility `UploadQueue` transfers 192 KiB through 64 KiB staging. GPU readback verifies bytes in the last chunk. `BumpAllocator`, `TextureAllocator` and `DeleteQueue` are used in the renderer. |
| Modern ImGui backend | Texture create/update/destroy, RGBA32/Alpha8 upload paths, clipping, `IdxOffset`, and `VtxOffset`. The GPU test exceeds 65,536 vertices and checks the resulting pixels. Three texture versions are submitted without intervening CPU waits and each image is checked. Texture updates use copy-on-write to preserve in-flight descriptors. |
| PNG export | Compose the scene, GPU highlight, ImGui text and images into an offscreen target. Read back RGBA and encode through SDL 3.4. The export test decodes the PNG and compares every pixel, including orientation, using a Unicode filename in a unique absolute temporary directory. |
| DPI/viewport mapping | CPU unit tests cover independent X/Y scale, nonzero viewport offsets, exclusive edges, invalid input, and scissor clamping. |

## Build and run

Requirements: the normal woby build dependencies, Vulkan SDK **1.4.357 or newer**,
standalone **Slang 2026.18.3 or newer**, and SDK `spirv-val`. The SDK's bundled
Slang may be too old. The prototype needs the upstream GPU feature profile,
including `VK_EXT_descriptor_heap`, `VK_KHR_device_address_commands`, and sample
rate shading for the experimental MSAA path.

On this development machine, `C:\tools\graphics-dev.ps1` selects the installed
Slang 2026.18.3 and Vulkan SDK 1.4.357.0. From the repository root:

```powershell
. C:\tools\graphics-dev.ps1
cmake --preset vs2026-vcpkg -DWOBY_BUILD_NOGRAPHICSAPI_PROTOTYPE=ON -DWOBY_TEST_NOGRAPHICSAPI_GPU=ON
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg -R woby_ngapi

# Interactive scenario selector, hover highlighting and PNG export
.\build\vs2026-vcpkg\bin\Debug\woby_nographicsapi_prototype.exe

# Deterministic headless acceptance checks, retaining PNG evidence
.\build\vs2026-vcpkg\bin\Debug\woby_nographicsapi_prototype.exe --self-test --output build\nographicsapi-captures

# The same GPU checks through a hidden SDL/Win32 presentation window
.\build\vs2026-vcpkg\bin\Debug\woby_nographicsapi_prototype.exe --window-test
```

On other machines set `VULKAN_SDK` and put standalone Slang and SDK tools on
`CMAKE_PROGRAM_PATH`, or pass `-DWOBY_SLANGC=C:/.../slangc.exe` and
`-DWOBY_SPIRV_VAL=C:/.../spirv-val.exe`. The default woby configuration has the
prototype **off** and does not download NoGraphicsAPI or require these tools.
GPU CTest registration is separately opt-in so unsupported CI runners can still
build and run the CPU tests. CI should use the existing `ninja-vcpkg` preset with
the same switches on a supported Windows GPU runner.

Shaders are compiled by Slang and validated by `spirv-val --target-env vulkan1.4
--scalar-block-layout` as build dependencies. They are loaded from the build tree;
this executable is a development experiment, not a portable release package.
The UI's Export button writes `nographicsapi-prototype.png` in the current
directory (or the directory supplied with `--output`).

## Upstream additions, reproducibility and limitations

The source archive is pinned to NoGraphicsAPI commit
[`ae017a2f545abc0847e546cc7e84139bf3cc4241`](https://github.com/sebbbi/NoGraphicsAPI/tree/ae017a2f545abc0847e546cc7e84139bf3cc4241)
with a SHA-256 check. Its MIT license and notices are retained in the fetched
source. `patch_upstream.cmake` applies a small, idempotent extension **only to the
private fetched source**:

- `TextureDesc.sample_count` and `GraphicsPSODesc.sample_count` (1 or 4).
- `ColorAttachment.resolve_view` for average color resolve.
- `GraphicsPSODesc.topology` for triangle lists or line lists.
- Require/enable Vulkan `sampleRateShading`; the circle shader uses per-sample
  interpolation so partial coverage is observable in the ID attachment.

These are **local experimental additions**, not features supported by the pinned
upstream API. The original Metal backend has not been extended, which is why
configuration explicitly rejects non-Windows platforms. The patch does not offer
a general sample-count capability API or integer/depth resolve modes.

This proves selected rendering mechanics on supported hardware. It does not prove
full woby migration feasibility, performance improvement, pixel parity with bgfx,
large-model scalability, world-space camera precision, annotation placement,
arbitrary OBJ import, screenshot tiling, or macOS/Linux parity. Texture updates
copy whole images for clarity rather than optimizing dirty rectangles. All work
uses the general queue; no cross-queue concurrency claim is made. Frame memory is
bounded at 16 MiB per frame, the shared texture heap is 768 MiB, descriptor capacity
is 256, and each drawable dimension is limited to 2048 pixels.

The next integration step would be a renderer adapter for real woby draw packets,
then image/selection comparisons against bgfx on actual models. Keeping bgfx as a
fallback remains appropriate until those tests and platform support exist.

## Verified on 2026-09-28

- NVIDIA GeForce RTX 3070 Laptop GPU, driver **616.92**, Windows x64.
- Visual Studio 2026 Debug build, Slang **2026.18.3**, Vulkan SDK **1.4.357.0**.
- Full Debug build completed without compiler warnings; all nine Slang entry
  points passed SPIR-V validation.
- `ctest --preset vs2026-vcpkg --parallel 2`: **653/653 passed** (the normal
  preset excludes the existing slow tests). This includes four prototype CPU
  test cases / 28 assertions, the headless GPU suite and the hidden-window suite.
- The 4× ID-edge test observed **six pixels with mixed sample IDs**, while the
  1× test observed zero. Large IDs, blending, occlusion, same-submission
  highlighting, scene replacement and PNG pixel comparisons all passed.
- The hidden-window suite also passed with `VK_LAYER_KHRONOS_validation` and
  `khronos_validation.validate_sync = true`, with no validation errors or warnings.
  Loader diagnostics separately confirmed that the validation layer was loaded.

Local evidence is under `build/`: `debug-build-final.log`, `unit-tests.log`,
`prototype-validation-final.log`, and `nographicsapi-window-captures/`. These
generated files are intentionally outside version control.
