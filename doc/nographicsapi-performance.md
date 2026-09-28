# Main versus NoGraphicsAPI: large-model FPS

Measured on 28 September 2026. The migration improves edge overlays on the four large models by 15–20%, but vertex overlays regress by 26–34%. Solid rendering of the largest model is close: 60.5 FPS on main versus 59.0 FPS after migration. Lighter solid scenes also lose some presented FPS; the empty-scene control suggests investigating presentation/frame pacing separately from geometry rendering.

## Builds and machine

- Main baseline: `97886f5fa86f09256b95237016fbbd3315670a73` (`97886f5`, Share indexed comparison vertices and separate mesh limits), the common ancestor from which the current work branched. Main has advanced since that revision.
- Baseline source was checked out without edits in `D:\.worktree\bgfx-baseline\woby`; Release built with `vs2026-vcpkg`, without compiler warnings. Tests were not rebuilt for this untouched historical checkout; all benchmark app processes exited successfully.
- Migration: the same packaged NoGraphicsAPI Release executable used for the preceding FPS measurements. Both application builds are version 0.21.3.
- Renderers observed at runtime: **bgfx / Direct3D 11 → NoGraphicsAPI / Vulkan**. This compares the actual application backends; it does not isolate the graphics library from the graphics API.
- NVIDIA RTX 3070 Laptop GPU, 8 GiB; driver 616.92; Ryzen 7 5800H; 64 GiB RAM; AC power. Both apps were verified on the NVIDIA GPU.

## FPS

Each cell is **original main → NoGraphicsAPI**. Higher is better.

| Model | Triangles (million) | Solid | Solid + edges | Solid + vertices |
|---|---:|---:|---:|---:|
| BusGameMap | 1.05 | 120.0 → 119.4 | 120.0 → 119.1 | 120.0 → 108.6 |
| Bennu | 17.87 | 120.0 → 107.0 | 56.4 → 64.8 | 85.0 → 62.8 |
| Powerplant | 12.76 | 120.0 → 107.9 | 33.7 → 39.3 | 38.7 → 25.6 |
| San Miguel | 9.98 | 119.9 → 112.7 | 51.4 → 61.3 | 60.2 → 42.6 |
| BearTrap Ground | 75.77 | 60.5 → 59.0 | 5.7 → 6.8 | 9.3 → 6.1 |

Change relative to main, calculated from unrounded measurements:

| Model | Solid | Solid + edges | Solid + vertices |
|---|---:|---:|---:|
| BusGameMap | -0.6% | -0.7% | -9.5% |
| Bennu | -10.8% | +14.8% | -26.1% |
| Powerplant | -10.1% | +16.7% | -33.9% |
| San Miguel | -6.0% | +19.1% | -29.2% |
| BearTrap Ground | -2.5% | +19.9% | -34.3% |

Empty-scene control: **119.62 → 110.53 FPS** (-7.6%). VSync is enabled, so the lighter-scene results include presentation pacing and should not be interpreted as uncapped GPU throughput. The observed values around 120 FPS do not establish a hardware refresh-rate limit.

## GPU time

Median GPU milliseconds reported by each backend, **main → NoGraphicsAPI**. Lower is better; backend timestamp scopes may differ, so use these as supporting measurements rather than an isolated shader benchmark.

| Model | Solid | Solid + edges | Solid + vertices |
|---|---:|---:|---:|
| BusGameMap | 2.33 → 2.16 | 2.87 → 2.63 | 2.73 → 3.09 |
| Bennu | 3.89 → 4.36 | 17.69 → 15.27 | 11.39 → 15.57 |
| Powerplant | 3.04 → 3.24 | 29.62 → 25.35 | 25.82 → 39.07 |
| San Miguel | 3.01 → 2.85 | 19.49 → 16.32 | 16.63 → 23.47 |
| BearTrap Ground | 16.35 → 16.57 | 175.46 → 146.48 | 107.54 → 164.21 |

For BearTrap, the edge GPU time improves from 175.5 to 146.5 ms, while the vertex mode increases from 107.5 to 164.2 ms. Both overlay runs report 100% GPU utilization. Powerplant's vertex mode rises from 25.8 to 39.1 ms while sampled GPU core clocks are 1950 MHz in both runs. These observations support prioritizing the vertex-marker rendering path for GPU profiling. They do not identify the exact shader or rasterization cause.

Solid GPU times are much closer than the light-scene FPS differences, and the empty-scene difference persists without model geometry. Frame pacing/presentation is therefore a separate investigation candidate, not a confirmed diagnosis.

## Method and verification

- The same measurement harness and mode sequence were used; only baseline metadata differs between script copies.
- Default 1280 × 720 desktop window, left scene pane visible at width 320, four-sample MSAA, VSync enabled. Grid, origin and dimensions hidden.
- Fixed isometric camera framed to each entire model. Solid shading remains on; edge and vertex modes add their respective overlay. Vertex markers are 4 pixels.
- One fresh process per model; import and background preparation finish before measuring. Four-second warm-up per mode, then a 12-second measurement window with performance queries approximately every 250 ms.
- FPS is the change in application frame count divided by elapsed wall time, using the first and last query midpoints. This is not a 1% low calculation.
- Verified matching model byte sizes, triangle/vertex/group counts, bounds, visibility flags, marker sizes, and exact camera payloads across both runs. All 30 model/mode measurements completed and all ten model app processes exited with code 0.
- Baseline ran after the previously measured migration build. One pass per backend, with normal dynamic GPU clocks; no confidence intervals or statistical significance claim. The large overlay differences are supported by GPU timing, but small differences should not be overinterpreted.
- No driver/power changes or injected Vulkan validation layers. Source OBJ files were read only. These measurements cover static whole-model views, not camera motion, close-ups, 4K, or uncapped rendering.

## Import times

Observed model-add operation, including GPU finalization. OS caches were not cleared, so this is not a cold-disk loading comparison.

| Model | Main seconds | NoGraphicsAPI seconds |
|---|---:|---:|
| BusGameMap | 0.27 | 0.30 |
| Bennu | 3.91 | 3.98 |
| Powerplant | 3.97 | 3.96 |
| San Miguel | 3.37 | 3.49 |
| BearTrap Ground | 23.01 | 24.19 |

## Saved evidence

The [comparison CSV](nographicsapi-performance.csv) contains unrounded FPS and GPU timing summaries. The measurements were collected through the application's `ctl` interface using the method above.

Local raw samples and the benchmark scripts are retained under `build/large-model-fps` (ignored build artifacts). `results.json` contains the migration measurements; `bgfx-baseline/results.json` contains the historical-main measurements. Each includes per-sample timings and GPU telemetry. The baseline source/build is retained in `D:\.worktree\bgfx-baseline\woby`.

Baseline executable SHA-256: `2029e46a6edb8235323f0e7f7633967ec640520f63bef23bdd7b648dfe9423dd`.

Migration executable SHA-256: `206f3b6769cb6c2c71443aa571b8af7898021c06f56b7b05f85c657ef6fc1b9a`.
