# OBJ compaction: CPU, upload, and GPU measurements

**Current status:** compaction has been removed from all application import
paths, together with its API, meshoptimizer dependency and comparison benchmark.
There is no setting to enable it. This document and the raw results preserve the
measurements that informed that decision; implementation details below describe
the application and benchmark at the time of the experiment.

Measured on 2026-09-27 using the five OBJ models in `D:/temp/obj_tests`.
This experiment compares the former final `compactMesh` pass with skipping
that pass. Both variants still map OBJ position/normal/UV index tuples to shared
render vertices. It does **not** compare indexed meshes with three independent
vertices per triangle.

**Compaction increased load-to-first-completed-draw time for every model.** Its
GPU memory savings were negligible on BearTrap, zero on Bennu and BusGameMap,
and about 10 MiB / 7 MiB on Powerplant / San Miguel. Most CPU allocation savings
were unused vector capacity, not resident RAM. Skipping compaction preserved
the rendered geometry in all checks.

The [raw results](obj-compaction-results.json) contain 30 primary runs plus four
separate pipeline-counter runs. Tables use the median of the three primary runs
per model/setting. `off → on` means compaction disabled followed by enabled.

## Load and upload times

| Model | CPU load off → on (s) | Compaction pass alone (s) | Upload off → on (s) | Load + upload + first completed draw off → on (s) |
|---|---:|---:|---:|---:|
| BearTrap | 29.822 → 37.811 | 7.863 | 2.245 → 2.290 | 32.078 → 40.118 |
| Bennu | 6.170 → 7.591 | 1.650 | 0.679 → 0.680 | 6.855 → 8.304 |
| Powerplant | 4.501 → 6.014 | 1.855 | 0.477 → 0.417 | 4.945 → 6.417 |
| San Miguel | 4.129 → 5.133 | 1.415 | 0.419 → 0.400 | 4.572 → 5.560 |
| BusGameMap | 0.245 → 0.287 | 0.059 | 0.045 → 0.050 | 0.293 → 0.335 |

The last column is a per-run sum before taking its median. Separate stage
medians do not necessarily add to a median total. GPU initialization and
validation are excluded, so this is a controlled pipeline comparison rather
than application launch time. Three samples are not a statistical confidence
interval. CPU-load ranges are retained in the raw data; for example BearTrap
was 28.278–31.904 s off and 37.655–38.816 s on.

The CPU-to-GPU path breaks down as follows. CPU copying is included in
preparation, and preparation plus execution/wait is included in upload total.

| Model | CPU staging allocation + copy off → on (ms) | Preparation + enqueue off → on (ms) | D3D11 execution + completion wait off → on (ms) |
|---|---:|---:|---:|
| BearTrap | 430.3 → 438.3 | 1693.3 → 1721.7 | 558.4 → 573.7 |
| Bennu | 101.8 → 103.9 | 501.8 → 507.3 | 178.1 → 173.2 |
| Powerplant | 100.7 → 102.7 | 230.2 → 242.5 | 243.5 → 174.9 |
| San Miguel | 83.7 → 79.3 | 269.9 → 260.9 | 140.4 → 139.5 |
| BusGameMap | 6.2 → 6.2 | 16.1 → 16.0 | 29.1 → 34.2 |

Even the largest observed median upload saving, about 60 ms for Powerplant,
does not repay its roughly 1.9 s compaction pass. Copying does not consistently
get faster when the payload shrinks by only a few percent. For BearTrap,
roughly 1.26 s of preparation is outside the staging-copy calls; this includes
validation and CPU point-range construction, which the production solid path
performs even when vertex sprites are disabled.

## Measured CPU memory

All values here are MiB of process memory, not just mesh arrays. Load-only
snapshots precede graphics initialization.

| Model | Private commit after load off → on | Resident working set after load off → on | Peak working set during load off → on | Peak private commit during load off → on |
|---|---:|---:|---:|---:|
| BearTrap | 4346.8 → 3783.5 | 3780.2 → 3780.2 | 9998.4 → 10206.9 | 10834.4 → 10997.3 |
| Bennu | 890.3 → 890.9 | 892.2 → 892.3 | 1927.5 → 2234.3 | 1996.8 → 2235.5 |
| Powerplant | 843.9 → 757.1 | 769.7 → 759.6 | 1724.1 → 1950.9 | 1964.4 → 2057.2 |
| San Miguel | 779.1 → 639.6 | 648.7 → 641.5 | 1681.7 → 1721.6 | 1873.5 → 1904.4 |
| BusGameMap | 64.0 → 56.7 | 59.6 → 59.7 | 142.0 → 145.3 | 151.3 → 153.0 |

Compaction raises peak loading memory in every case. The remap and replacement
vertex array coexist with the old vertex allocation, and the parsed OBJ and
tuple map are still alive at this point. Bennu is particularly clear: no
vertices disappear, retained memory is effectively identical, but compaction
adds about **307 MiB to peak resident memory** and 1.65 s of work.

BearTrap's retained commitment falls by about 563 MiB while its resident set
stays at 3780 MiB: most of that allocation was untouched spare capacity.
Powerplant and San Miguel save about 10 MiB and 7 MiB of resident mesh-loading
memory, consistent with the vertices actually removed.

After upload and rendering, total resident process memory was:

| Model | Resident working set off → on (MiB) |
|---|---:|
| BearTrap | 4107.9 → 4098.8 |
| Bennu | 1039.8 → 1039.7 |
| Powerplant | 917.4 → 916.4 |
| San Miguel | 800.9 → 793.9 |
| BusGameMap | 139.3 → 139.9 |

These totals also include the renderer, CPU point indices and driver behavior,
so their small deltas should not all be attributed to vertex removal. During
BearTrap staging, the process working set rises by about 2186 MiB; 2039 MiB is
the copied vertex/index payload, with point indices and other preparation
allocations accounting for the remainder. The staging copy is released when
bgfx consumes it. All four large models reach their overall peak during
loading. BusGameMap instead peaks after graphics startup, around 164 MiB off /
165 MiB on resident, and 283 MiB off / 275 MiB on committed.

An allocation side effect appears in Powerplant's CPU point-index cache:
capacity rises from **41.78 MiB off to 60.79 MiB on**. The cache reserves the
global render-vertex count, but stores vertices per group. After compaction,
10,939,920 per-group point entries exceed the 10,624,089-vertex reservation,
triggering vector growth. Without compaction, 10,953,627 entries fit the initial
reservation exactly. This is another reason global vertex savings need not
translate directly into savings for every runtime allocation.

## Measured GPU memory and rendering

The payload column is the exact combined vertex/index size. The local-usage
column is the measured change in DXGI local usage after upload completion. Shared
usage is measured after rendering relative to the same fixed baseline.

| Model | Payload off → on (MiB) | Dedicated/local usage increase off → on (MiB) | Shared/non-local usage increase off → on (MiB) |
|---|---:|---:|---:|
| BearTrap | 2039.456 → 2039.454 | 2039.563 → 2039.500 | 118.711 → 110.211 |
| Bennu | 477.099 → 477.099 | 477.188 → 477.188 | 50.961 → 50.961 |
| Powerplant | 480.296 → 470.239 | 480.375 → 470.313 | 43.273 → 52.023 |
| San Miguel | 389.531 → 382.324 | 389.563 → 382.375 | 54.023 → 54.023 |
| BusGameMap | 28.776 → 28.776 | 28.875 → 28.875 | 14.742 → 14.867 |

The measured local allocations closely match the payload. Shared allocations
include driver overhead and do not monotonically follow the vertex count.

| Model | GPU frame median off → on (ms) | Range of three run medians, off / on (ms) | Synchronized frame wall median off → on (ms) |
|---|---:|---:|---:|
| BearTrap | 15.640 → 15.651 | 15.624–15.650 / 15.639–15.728 | 16.168 → 16.651 |
| Bennu | 5.198 → 5.297 | 5.137–5.356 / 4.911–5.318 | 15.787 → 15.937 |
| Powerplant | 4.610 → 2.513 | 4.547–4.675 / 2.511–2.627 | 15.808 → 15.953 |
| San Miguel | 1.942 → 3.034 | 1.937–3.343 / 1.941–4.258 | 3.837 → 15.442 |
| BusGameMap | 0.205 → 0.205 | 0.205–0.205 / 0.205–0.205 | 0.387 → 0.414 |

These timestamp results are workload observations, not reliable viewer-FPS
predictions. Clock state, driver scheduling and explicit synchronization were
not controlled. Wall times often approach a scheduling interval despite much
shorter GPU timestamp intervals. San Miguel's wide variation does not establish
a rendering regression. BearTrap, Bennu and BusGameMap show no useful benefit.
Powerplant consistently favors compaction in GPU timestamps, but the magnitude
is not stable: an additional pair measured **2.936 → 2.595 ms**, much smaller
than the primary sweep's approximately 45% reduction.

To distinguish less shader work from timing variation, a separate D3D11
pipeline-statistics query was added **after** the timing samples. One additional
off/on pair was run for Powerplant and San Miguel; those runs are excluded from
the primary medians above.

| Model | Vertex-shader invocations off → on | Reduction | Pixel-shader invocations, both modes |
|---|---:|---:|---:|
| Powerplant | 13,667,292 → 13,615,944 | 0.376% | 39,044 |
| San Miguel | 14,459,690 → 14,454,508 | 0.036% | 166,758 |

The input primitive and input vertex counts are also identical in both modes.
San Miguel's extra pair measured **1.94136 → 1.94048 ms**, effectively equal.
The counters demonstrate a small reduction in repeated vertex work, but do not
explain or substantiate a general 45% speedup. Memory locality could also
contribute; this experiment does not isolate it from clock/driver effects.
Counter definitions are documented in Microsoft's
[D3D11 pipeline statistics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_pipeline_statistics).

For fast initial viewing, these measurements favor avoiding unconditional
value compaction, especially for BearTrap, Bennu and BusGameMap. Following the
experiment, final compaction and its benchmark were removed entirely. No setting
was added to enable it. Capacity trimming remains a separate, unmeasured option
for future optimization.

## Vertex counts and exact memory savings

| Model | Vertices before | Vertices after | Removed | CPU vertex capacity saved (MiB) | GPU vertex payload saved (MiB) |
|---|---:|---:|---:|---:|---:|
| BearTrap | 38,414,391 | 38,414,328 | 63 (0.00016%) | 562.18 | 0.0019 |
| Bennu | 8,933,524 | 8,933,524 | 0 | 0 | 0 |
| Powerplant | 10,953,627 | 10,624,089 | 329,538 (3.01%) | 86.67 | 10.06 |
| San Miguel | 9,021,391 | 8,785,228 | 236,163 (2.62%) | 139.30 | 7.21 |
| BusGameMap | 547,469 | 547,469 | 0 | 7.82 | 0 |

MiB means 2^20 bytes. These are exact allocation/payload calculations, not
differences between sampled process counters. Every render vertex is 32 bytes.
Compaction preserves the triangle-index count and its 32-bit element size, so
the index buffer contributes **zero** savings.

The five models together lose only **17.27 MiB of GPU vertex data**, but their
CPU vertex-vector capacities shrink by **795.98 MiB**. About **778.71 MiB** of
that CPU saving is unused capacity rather than duplicate vertex data. Source
positions and source indices are unchanged. Render indices are remapped while
preserving triangle order and corner attributes.

Do not read capacity savings as an equal reduction in physical RAM. Windows
private commitment includes these allocations, but most spare capacity was not
resident. The sampled process working set and its peak are reported separately
below. After graphics initialization, process commitment also reflects driver
allocations; CPU commitment and dedicated GPU usage should not be added as if
they were two measurements of physical system RAM.

This happens because the loader initially reserves the OBJ position count,
then grows the render vertex vector when normals or UVs require more index
tuples. The final compaction builds a new exactly sized vector, incidentally
discarding spare capacity. The GPU upload already uses `vertices.size()`, not
`vertices.capacity()`: unused CPU capacity is never uploaded.

Capacity trimming and value deduplication should therefore be evaluated as
separate operations in a future optimization. A trim-only pass could retain
most of these CPU capacity savings without hashing every vertex or rewriting
every triangle index. Its runtime has not been measured in this experiment.

## Method

- Ryzen 7 5800H, 8 cores / 16 threads, 64 GiB RAM; NVIDIA GeForce RTX 3070
  Laptop GPU, 8 GiB dedicated memory, driver 546.30; SKHynix NVMe storage.
- MSVC v145, VS 2026 `vs2026-vcpkg` preset, Release `/O2 /Ob2 /DNDEBUG`.
  Installed meshoptimizer 1.1 and bgfx 1.129.8940-496#1.
- Three fresh processes per model per setting, run serially. Order alternates
  off/on, on/off, off/on. The input is read once before each pair; there is no
  cache eviction. RapidOBJ retains its production native I/O behavior.
- CMake generates instrumented copies of the production OBJ loader and mesh
  finalization code. Only the final compaction call is conditional. Normal
  generation, bounds, triangulation, index-tuple mapping and source provenance
  are unchanged. No production import behavior is modified.
- CPU load time ends when `loadObjMesh` returns. Stage timing, process working
  set, private commit and their process peaks are recorded. Exact vector sizes
  and capacities distinguish live vertex data from spare allocation capacity.
- Full triangle-corner vertex-attribute hashes and source-data hashes are
  computed after the load timer. These checks are deliberately excluded from
  timings, as are GPU initialization, shader loading, screenshot readback and
  process startup/shutdown.

## What the GPU measurements include

The probe extracts the production solid-mode `createGpuMesh`, point-range
preparation, vertex layouts and destruction code. It records the two actual
`bgfx::copy` calls, which allocate CPU staging memory and copy the vertex and
triangle-index arrays into it. Preparation also includes index validation,
per-group point indices, temporary per-vertex group stamps, and enqueueing.

For controlled synchronization, bgfx's API and render work run on one thread.
After the frame processes native D3D11 buffer creation, the probe inserts a
`D3D11_QUERY_EVENT`, flushes, and waits until `GetData` reports completion.
Therefore **execute + wait** includes driver allocation, transfer/submission and
synchronization. It is not a measurement of pure PCIe DMA bandwidth, and it must
not be added to the total again. **Upload total** includes preparation, the CPU
staging copies and execute + wait. See Microsoft's
[query definitions](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/d3d11/ne-d3d11-d3d11_query.md).

GPU memory is measured using the current process's DXGI local and non-local
usage, before upload and after completion. A fixed baseline already contains
the renderer, shaders and 1280×720 offscreen color/depth/readback resources.
Allocation granularity and driver overhead can make the measured delta differ
from exact buffer payload. These are process accounting counters, not the
whole GPU's memory usage. See
[QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo).

Rendering uses the production mesh shaders, all solid groups, one deterministic
camera fitted to the bounds, depth testing, no culling, no MSAA or vsync, and
no UI or overlays. The first completed draw is timed separately. After at least
20 warm-up draws and two seconds, each process records 60 frames, using bgfx GPU timestamps and
an explicit completion wait per frame. Only distinct valid GPU frame numbers
are counted. Frame times here describe this controlled camera and workload;
they are not application FPS. The experiment excludes wireframe buffers,
vertex sprites, annotation caches and the rest of the viewer's UI state.

Every run also reads back the rendered pixels and requires nonempty geometry.
The sweep checks matching pixel, triangle-corner and source hashes across all
on/off runs for each model.

## Historical benchmark coverage

The removed benchmark was Windows-only and selected the NVIDIA adapter, recorded
in each result. Round one saved on/off PPM images alongside the JSONL. Its
small contract fixture covered duplicate values with different OBJ indices,
UV seams, missing normals, multiple groups, exact source preservation, invalid
arguments, sweep output and failures. All fixture paths derived from one unique
temporary directory, with cleanup after failure. The graphics contract also
checked buffer byte counts and identical rendered pixels.

## Validation at measurement time

- The Release benchmark built without compiler warnings. Both Release
  compaction contracts passed after the final counter instrumentation.
- All 30 primary and four supplemental runs succeeded. Triangle-corner,
  source-data and pixel hashes match for each model across all runs. All five
  saved off/on image pairs also match byte-for-byte; the model views were
  visually inspected.
- `cmake --build --preset vs2026-vcpkg` built the complete Debug project without
  warnings.
- `ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure` passed
  **627/627 tests**, including slow and graphics tests, in **236.49 seconds**.
- No production loading or rendering behavior was changed. No tests were added
  for document content.

Local build/test logs are `build/compaction-debug-build.log` and
`build/compaction-ctest.log`. Primary raw JSONL and images are under
`build/compaction-final`; supplemental counter runs are under
`build/compaction-counters`. The earlier `build/compaction-results` directory
contains exploratory short-warm-up runs and is excluded from this report.
