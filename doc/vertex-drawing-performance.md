# Vertex marker shader performance

Measured on 27 September 2026 against production source `b9fc2dc`, using the
five OBJ models in `D:\temp\obj_tests`. The shader approach substantially reduces
first-enable latency and buffer payload. Steady drawing is workload-dependent:
the largest solid-plus-markers scene became 2.7 times faster, while Bennu became
16% slower. This experiment adds an opt-in benchmark; the viewer still uses its
existing renderer.

The proposed path reads positions from the existing mesh vertex buffer, uploads
the compact group vertex-ID list, and generates four quad corners per instance.
It eliminates expanded marker geometry, but still uploads four bytes per marker.
The existing CPU mesh and point-ID list remain available to picking, analysis,
and other consumers described in the [data audit](vertex-drawing-shader-research.md).

All comparisons below are **current expanded geometry → instanced shader**.
Times are milliseconds, lower is better, and each number is the median of three
independent run medians. MiB and GiB use powers of 1024.

The most useful interactive rendering comparison includes both the solid mesh
and four-pixel opaque vertex markers. These are GPU times for the submitted
scene, excluding the viewer UI and presentation:

| Model | Current GPU frame | Shader GPU frame | Change in time | Range across runs, current → shader |
|---|---:|---:|---:|---|
| BusGameMap | 0.645 | 0.687 | 6.5% slower | 0.633–0.658 → 0.669–0.694 |
| Bennu | 9.859 | 11.415 | 15.8% slower | 9.660–10.263 → 11.083–11.700 |
| Powerplant | 18.352 | 17.869 | 2.6% faster | 17.743–18.420 → 17.578–18.735 |
| San Miguel | 13.439 | 13.212 | 1.7% faster | 13.324–13.542 → 13.101–13.302 |
| BearTrap | 192.977 | 71.039 | **63.2% faster** | 157.328–193.014 → 70.145–71.082 |

BearTrap's improvement is substantial and present in every run. Mean elapsed time
per completed frame also fell from 159–197 ms to 70–71 ms. The marker pass alone
did not show a corresponding improvement: its median was 53.445 → 54.679 ms in
this scene, with one current-path run reaching 141.106 ms. The current path's
stall shifted between passes across runs.

Reduced GPU memory pressure is a plausible explanation, **not a measured causal
finding**. BearTrap's main-plus-marker buffer payload falls from 5.71 GiB to
2.13 GiB on this 8 GiB GPU. Adapter-wide memory counters also fell, but they
include other applications and cannot establish per-process residency or
evictions. A GPU residency trace would be needed to attribute the stall precisely.
Small improvements on Powerplant and San Miguel should not be generalized to
other views, drivers, or hardware; Powerplant's run ranges overlap.

First enable shows a much more consistent benefit. These measurements start
after loading the model and creating its main mesh buffers and group membership.
CPU preparation includes buffer enqueueing. Ready time extends through submission
and a GPU readback fence; first-draw time additionally forces an actual marker
draw so deferred driver work is exercised.

| Model | Markers | CPU preparation | Buffers ready | First draw completed |
|---|---:|---:|---:|---:|
| BusGameMap | 547,469 | 42.25 → 0.61 | 66.68 → 2.23 | 68.97 → 4.73 |
| Bennu | 8,933,524 | 685.44 → 8.16 | 917.74 → 17.60 | 924.08 → 27.20 |
| Powerplant | 10,953,627 | 851.95 → 8.94 | 1,142.80 → 19.69 | 1,164.45 → 45.97 |
| San Miguel | 9,021,669 | 662.62 → 9.02 | 899.35 → 18.05 | 925.89 → 47.48 |
| BearTrap | 38,414,391 | 2,816.31 → 32.35 | 4,313.03 → 65.97 | 4,378.81 → 130.17 |

Buffer preparation through readiness is approximately **30–65 times faster**;
through the first completed draw it is **15–34 times faster**. These are marker
enable costs, not total file-load times. Membership construction and the original
mesh upload still occur. Existing markers are retained after first enable, so
this improvement does not apply to every visibility toggle or every frame.

Marker buffer payload is exactly `104P → 4P` bytes, a **96.15% reduction**, where
`P` counts unique render vertex IDs within each group. Shared IDs across groups
still produce separate markers, preserving current behavior.

| Model | Current marker payload, MiB | Shader marker payload, MiB |
|---|---:|---:|
| BusGameMap | 54.30 | 2.09 |
| Bennu | 886.05 | 34.08 |
| Powerplant | 1,086.40 | 41.78 |
| San Miguel | 894.79 | 34.41 |
| BearTrap | 3,810.02 | 146.54 |

These are exact application buffer sizes, not measured resident VRAM. Eliminating
CPU expansion also removes its large temporary vertex/index arrays. Loader,
CPU geometry, upload copies, allocator overhead, and driver allocations make
process-memory counters a separate quantity.

Isolating the marker pass makes the steady-state tradeoff clearer. Four-pixel
opaque markers are slower on BusGameMap and Bennu, with only small improvements
on the other models. Eight-pixel transparent markers are broadly unchanged:

| Model | Opaque markers only, GPU ms | Transparent markers only, GPU ms |
|---|---:|---:|
| BusGameMap | 0.473 → 0.516 | 1.349 → 1.343 |
| Bennu | 5.317 → 7.023 | 14.014 → 14.375 |
| Powerplant | 16.276 → 16.006 | 55.010 → 55.035 |
| San Miguel | 13.127 → 12.963 | 39.676 → 39.688 |
| BearTrap | 55.819 → 54.770 | 175.877 → 176.730 |

Bennu's opaque marker pass costs approximately 32% more. The new path adds
indirect buffer fetches and changes vertex scheduling, while retaining the same
fragment coverage. Those are possible reasons for the regression, not a
hardware-counter diagnosis. Removing geometry uploads cannot remove the cost
of drawing millions of overlapping circles. CPU submission also did not improve;
for example, San Miguel's 2,203 groups took 0.551 → 0.614 ms for the combined scene.

A second shader variant generated six vertices per marker in one non-instanced
triangle list. It uses the same compact buffers and produces the same pixels.
This tests whether removing tiny instances improves performance:

| Model | Opaque marker GPU ms, current / instanced / non-instanced | Combined scene GPU ms, non-instanced |
|---|---:|---:|
| BusGameMap | 0.473 / 0.516 / 0.515 | 0.694 |
| Bennu | 5.317 / 7.023 / 6.064 | 10.331 |
| Powerplant | 16.276 / 16.006 / 17.040 | 19.662 |
| San Miguel | 13.127 / 12.963 / 14.614 | 14.444 |
| BearTrap | 55.819 / 54.770 / 59.662 | 76.122 |

The non-instanced variant mitigates Bennu's regression but has no consistent
advantage. The four-corner instanced path is the stronger initial candidate for
reducing memory and enable latency. The measurements do not justify promising a
universal frame-rate increase or selecting either variant universally on other
devices. A production integration should preserve a compatible fallback and
validate the actual windowed rendering configuration.

The experiment used a Ryzen 7 5800H, approximately 64 GiB system memory, an
RTX 3070 Laptop GPU with 8 GiB VRAM, NVIDIA driver 546.30, and the D3D11 backend.
Both builds used the `vs2026-vcpkg` preset; timing used Release. There were
**45 serial fresh-process runs**: five models, three implementations, three
rounds. Variant order rotated each round. Every process measured all three
scenarios after at least one second and 12 warmup frames, sampling for at least
two seconds and 30 submitted frames. GPU samples were deduplicated by completed
frame ID; the archive records sample counts and run ranges. No build or test
suite ran alongside the timed sweep.

All scenes used a fixed oblique camera fitted to the bounding sphere, all groups,
deterministic group colors, and a 1280×720 offscreen RGBA8/D24S8 target. Opaque
markers were four pixels across; transparent markers were eight pixels with
opacity 0.4. VSync and MSAA were disabled. The viewer normally uses 4× MSAA and
VSync, so this is closer to its screenshot path than a full interactive session.
Picking, UI, annotations, camera motion, and presentation were excluded. Results
can change with view, point size, overlap, MSAA, driver, GPU, or available memory.
Clocks and thermals were not pinned. Other graphics backends were not runtime
benchmarked.

Correctness checks retained the same ordered point membership and point counts.
All **90 real-model image comparisons were pixel-identical**: two shader variants
against current rendering, three scenarios, five models, three rounds. A separate
synthetic contract test covered shared IDs, hard-normal seams, coincident points,
repeated corners, unused source vertices, occlusion, transparency, perspective and
orthographic cameras, and 1/4/8/40-pixel markers. Both variants matched exactly.
These checks do not replace production validation of nested transforms, DPI,
resizing, clipping boundaries, or other backends.

The full Debug build and Release benchmark build completed without compiler
warnings. The full suite passed **633/633** before the final non-instanced variant
and analysis-test registration. After those additions, both focused Debug tests
passed, including six analysis unit tests and the graphics contract for both
variants; the Release graphics contract also passed. No production renderer
behavior changed, and no document-content tests were added.

The [benchmark README](../tests/vertex_drawing/README.md) gives build commands,
timing definitions, and reproduction instructions. The checked-in
[result archive](vertex-drawing-performance-results.json) contains all 45 run
records, both aggregate comparisons, memory counters, image hashes, and all
pixel-comparison results. Local raw captures and logs are in
`build/vertex-drawing-results/`; build and full-suite logs are
`build/vertex-benchmark-variants-debug.log`,
`build/vertex-benchmark-variants-release.log`, and
`build/vertex-benchmark-tests.log`.
