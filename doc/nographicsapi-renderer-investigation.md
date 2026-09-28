# Vertex rendering and frame pacing investigation

Investigated on 29 September 2026, following the [large-model comparison](nographicsapi-performance.md).
Production revision: `188efd8`. Historical main: `97886f5`.
RTX 3070 Laptop GPU, NVIDIA 616.92, Windows, AC power.

## Adopted follow-up: original pixel coverage

Following this investigation, the user chose the original renderer's pixel
coverage. The production shared UV interface now uses ordinary interpolation,
including both the display and marker-picking fragment shaders. This restores
the old circle cutout and removes its per-sample shading cost. MSAA geometry,
depth, and per-sample ID lookup remain enabled; FIFO presentation is unchanged.

The GPU regression now verifies the complete expected circle footprint for
both ordinary and MRT picking shaders, at one and four samples. The earlier
partial-circle-coverage expectation has been replaced to reflect this explicit
behavior choice. The investigation and alternatives below describe the state
before that decision; the coverage-mask prototype was not adopted.

The adopted Release build was measured on all five models with solid shading
and vertex markers enabled, using the same cameras, 1280 x 720 window, 4-pixel
markers, four-sample MSAA, four-second warm-up, and 12-second measurement window.
Geometry counts, camera payloads, and marker sizes matched the original run.

| Model | Initial migration FPS | Pixel coverage FPS | Change | Historical main FPS |
|---|---:|---:|---:|---:|
| BusGameMap | 108.6 | 120.0 | +10% | 120.0 |
| Bennu | 62.8 | 83.1 | +32% | 85.0 |
| Powerplant | 25.6 | 38.9 | +52% | 38.7 |
| San Miguel | 42.6 | 61.8 | +45% | 60.2 |
| BearTrap Ground | 6.1 | 9.9 | +62% | 9.3 |

Both builds were warning-free. All 668 Debug tests and six targeted Release
checks passed, including the vertex screenshot smoke and GPU picking tests.
Vulkan shaders compiled and passed `spirv-val`; the point, picking, comparison,
and ImGui fragment inputs were also checked for absence of sample interpolation.
All five benchmark processes exited successfully. The new portable Release ZIP
was verified against its manifest. Logs, raw measurements, a comparison CSV,
and the updated package are under `build/pixel-coverage`.

## Findings

The vertex regression is primarily fragment sample shading, not additional mesh
vertices or CPU submission work. The migrated shader evaluates the circular
cutout at each MSAA sample; the old shader evaluated it once per pixel. A
controlled shader substitution recovers the old renderer's Powerplant FPS.

The lighter-scene FPS loss reproduces in empty scenes. Instrumentation and
PresentMon narrow it to the current Vulkan FIFO presentation path on this
machine. Increasing presentation contexts from two to three does not fix it.
Changing to mailbox removes the throughput limit, but changes the Windows
presentation path and generates substantial excess rendering. The exact
driver/compositor scheduling cause remains unresolved.

At the end of the investigation, no experimental presentation or shading changes
had been adopted. The subsequent pixel-coverage decision is described above.

## Vertex rendering: controlled experiments

The shared `VertexOutput` in `shaders/native/woby.slang` declares
`sample float2 uv`. Disassembly of `fs_point_sprite.bin` confirms SPIR-V
`OpCapability SampleRateShading` and a `Sample` decoration on its UV input.
The fragment shader uses that UV to discard samples outside the circle.
Vulkan consequently evaluates this coverage per sample. This is documented in
the [Vulkan sample-shading specification](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-sampleshading).

The historical `point_sprite.frag.sc` uses ordinary interpolated UVs instead.
Both renderers draw the same four-vertex instanced quad for each point. The
per-sample behavior was introduced deliberately in the prototype so partial
circle coverage is represented in the picking attachment; it is a quality and
coverage difference as well as a performance difference.

Powerplant, fixed whole-model camera, 1280 x 720, four-sample MSAA, 4-pixel
markers, solid shading enabled. Fresh process per run; four-second warm-up and
12-second measurement window. FPS is application frame-counter delta / wall time.

| Variant | Solid FPS | Edges FPS | Vertices FPS | Vertex-mode GPU ms |
|---|---:|---:|---:|---:|
| Production NoGraphicsAPI | 110.08 | 39.66 | 25.72 | 39.02 |
| Remove shared UV `sample` qualifier | 108.78 | 39.39 | 38.66 | 25.51 |
| Explicit coverage mask, point fragment shader only | — | — | 34.05 | 29.09 |
| Production repeated after experiments | — | — | 25.63 | 38.94 |
| Historical main, previous measurement | 119.97 | 33.65 | 38.68 | 25.82 |

Solid and edge GPU times stayed essentially unchanged in the first shader
experiment (3.22 ms and 25.2 ms respectively). The vertex-mode change is about
50% more FPS, with 35% less GPU time. Simply removing the qualifier changes
circle boundaries and therefore is not a coverage-preserving optimization.

The mask prototype uses `EvaluateAttributeAtSample` for the four sample
positions and emits `SV_Coverage`, evaluating the fragment once per pixel.
It improves FPS by about 32% over production while retaining partial coverage
in the targeted GPU test. It is a proof of concept, not a finished implementation:
it currently assumes four samples and only replaces the ordinary point fragment
shader. A production implementation must also handle single-sample targets,
the MRT marker-ID path, transparent overlaps, depth/occlusion, and Metal.
The test does not establish exhaustive pixel equivalence across those cases.

The new `Native circular markers retain partial sample coverage with four sample
MSAA` GPU test draws a pixel-aligned quad, reads back resolved pixels, and checks
that the circular cutout generates partial coverage. Results:

- Existing production shader: passes.
- Explicit-coverage-mask prototype: passes.
- Pixel-rate shader without a coverage replacement: fails, as intended.

This negative control verifies that the test detects the tempting quality-losing
shortcut. The original shaders were restored after the experiment.

## Frame pacing: CPU and display evidence

Fresh empty-scene A/B runs reproduced the earlier difference:

| Build | Run 1 FPS | Run 2 FPS |
|---|---:|---:|
| Historical main | 119.90 | 119.97 |
| Production NoGraphicsAPI | 108.55 | 112.93 |

Temporary timers around frame-resource reuse, `gpu::acquire`, command recording,
and `gpu::submit_and_present` measured the following means after warm-up:

| Presentation contexts | Frame reuse wait ms | Acquire ms | Record ms | Submit/present ms | FPS |
|---|---:|---:|---:|---:|---:|
| 2 (current default) | 0.047 | 8.104 | 0.129 | 0.220 | 111.75 |
| 3 | 8.207 | 0.024 | 0.106 | 0.206 | 111.21 |
| 2 repeated | 0.049 | 8.135 | 0.128 | 0.213 | 112.00 |
| 3 repeated | 8.310 | 0.023 | 0.108 | 0.210 | 109.48 |

With three contexts the wait moves to frame-resource reuse. It does not remove
the underlying pacing limit, so a two-versus-three-context mismatch is ruled
out as a sufficient explanation or fix. Ordinary CPU scene/UI work is small.

PresentMon 2.6.0 captured the benchmark processes by PID without changing driver
settings. Both baseline and FIFO migration were observed as `Composed: Flip`,
with DXGI present events and sync interval 1. The migration's application
renderer still reports NoGraphicsAPI/Vulkan; the Windows/driver presentation
events do not mean the application rendered through D3D11.

| PresentMon metric | Historical main | NoGraphicsAPI FIFO |
|---|---:|---:|
| Application FPS during trace | 119.89 | 109.52 |
| Mean interval between presents, ms | 8.34 | 9.14 |
| Median interval between display changes, ms | 8.33 | 8.34 |
| 95th percentile interval between display changes, ms | 11.09 | 16.66 |
| Mean present-to-display delay, ms | 38.79 | 14.81 |

The FIFO run has about 130 display intervals near 16.6–16.7 ms during the
12-second trace, whereas main usually follows the approximately 8.3 ms
compositor cadence. The shorter present-to-display delay also shows why simply
adding buffering could trade latency for FPS. These are presentation metrics,
not end-to-end input latency measurements.

A mailbox experiment changed only the pinned dependency's presentation-mode
constant, retaining production shaders. It achieved about 1,396 application
FPS in the empty scene. PresentMon reported a different path,
`Composed: Copy with GPU GDI`, and most generated frames were not displayed.
This confirms substantial rendering headroom and a presentation-mode-dependent
limit; it does not establish a 1,396 Hz display or justify an uncapped default.
The experiment was reverted.

This machine also has a Meta Virtual Monitor adapter installed and two active
physical-monitor IDs reported by WMI; the NVIDIA controller reports 165 Hz,
while the captured compositor cadence is approximately 120 Hz. These observations
are not sufficient to assign the issue to a particular monitor. A follow-up
should record the exact target display and repeat on the intended physical
display before attributing the remaining scheduling behavior to a driver bug.

## Original recommendations before the pixel-coverage decision

1. Replace full per-sample circular-marker shading with an explicit coverage
   mask or another coverage-preserving method. Handle 1x/4x targets and both
   ordinary and picking shaders together, with occlusion/transparency tests.
2. Remove sample interpolation from unrelated shader interfaces where sample
   evaluation is unnecessary; the current shared struct also affects ImGui,
   comparison shading, and textured composites.
3. Investigate a paced mailbox path or presentation timing controls, measuring
   displayed intervals, latency, and GPU utilization together. Keep FIFO as a
   fallback and query mode support. An uncapped mailbox switch and increasing
   presentation contexts are not validated solutions for this laptop.

## Evidence and reproduction notes

The original packaged Release executable and assets are unchanged. Its executable
SHA-256 remains `206f3b6769cb6c2c71443aa571b8af7898021c06f56b7b05f85c657ef6fc1b9a`.
All diagnostic source instrumentation and dependency presentation changes were
reverted. Only the coverage regression test and documentation remained as source
changes from this investigation.

Raw evidence is retained locally under `build/renderer-investigation` (ignored):

- `model.py`, `empty.py`, `empty_presentmon.py`: measurement harnesses.
- `before-powerplant`, `pixel-powerplant`, `mask-powerplant`,
  `before-powerplant-repeat`: raw samples and GPU telemetry.
- `before-empty-*`, `main-empty-*`, `profile-*-empty*`: A/B data and timers.
- `native-presentmon`, `main-presentmon`, `mailbox-empty`: ETW-derived CSVs.
- `pacing-instrumentation.patch`: temporary profiling implementation.
- `pixel-rate.slang`, `coverage-mask.slang`: experimental shader sources.
- `coverage-test-*.log`: positive and negative coverage controls.

Experiments used the existing benchmark settings, dynamic GPU clocks, and no
injected validation layers. Shader variants were compiled by Slang and checked
by `spirv-val`. The original main and migration runs used identical geometry
and camera settings. Individual variant timings are short measurements rather
than confidence intervals; the repeat controls and GPU timings support the
large observed differences.

## Final validation

- Restored production Debug and Release builds completed without compiler warnings.
- The full Debug run passed 664/668 checks. Four headless smoke checks initially
  detected native windows with implicit Vulkan layers enabled; all four passed
  when rerun with `VK_LOADER_LAYERS_DISABLE=~implicit~`, matching the benchmark
  environment. No headless assertions were weakened.
- All four production GPU regression checks passed in Release, including the
  new circular-coverage test, resource lifetime, empty-frame submission, and
  marker picking.
- The explicit-mask and pixel-rate negative-control runs used temporary shader
  substitutions with automatic restoration. Final validation used production
  assets.

Logs: `debug-build.log`, `debug-tests.log`, `debug-tests-rerun.log`,
`restored-release-build.log`, and `restored-release-gpu-tests.log`, all under
`build/renderer-investigation`.
