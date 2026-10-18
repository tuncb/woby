# GM input and PLAXIS difference mesh investigation

Current implementation: the fast approximate mesh calculation is the only
difference method. The method selector, voxel calculation, resolution checks,
and automatic surface closure were removed at the user's request. The findings
below describe the investigation, including methods that are no longer available.


The original saved comparison in `D:/temp/GM-data/test.woby` cannot reliably measure small
changes between geological units. There are four separate problems: its grid
is too large at the saved resolution, winding evaluation is slow, its input
membership removes unit identity, and its saved placement adds a vertical offset.
Direct mesh also rejects the three unit pairs tested here.

The requested result must include changes to internal geological boundaries.
Increasing the grid limit alone will not meet that requirement.

## Single-method verification

The Release build passed without compiler warnings after removal of the other
methods. All 91 default CTest entries passed. The separate difference CLI test
passed in 3.95 seconds. Tests cover thin rotated differences, internal unit
boundaries, cavities, coordinates, canceled and stale workers, display settings,
save/load, export, and rejection of the removed CLI options.

All nine geometric unit matches produced results. Calculation time was
0.145-4.276 seconds per pair, 18.819 seconds in total. These times exclude file
loading and input assembly. This was a verification run; a brief CLI test ran
concurrently, so use the earlier isolated measurements for performance comparison.

The prepared `test-unit-approximate.woby` scene was saved with the simplified
settings. All nine comparisons remain; only Rowan is created and displayed on
load. A fresh render contains both blue and orange result geometry. The original
`test.woby` hash is unchanged. Build logs, test logs, pair results, and the render
are in [`single-method`](benchmarks/gm-difference-20261018/single-method).

## Fast approximate mesh follow-up

The user accepts approximate results, does not want mesh repair, and requires
permissively licensed dependencies. The investigation introduced
**Approximate mesh (fast)**, originally saved as `approximate_mesh`. It uses the existing
Manifold 3.5.2 dependency, whose installed licence is Apache 2.0. No dependency
was added. The separate CGAL repair experiment was stopped.

This method performs both Boolean differences on the input triangles without
a voxel grid. It centers the working coordinates to reduce numerical
cancellation. It accepts surface intersections without running the exact
intersection detector. It checks library status, finite output coordinates,
closed and consistently oriented output edges, positive input volumes,
non-negative output volumes, and basic volume consistency. The consistency
allowance is `1e-7` times the larger input volume. It is a rejection guard, not
an accuracy bound. Direct mesh retains its exact intersection checks.

The result is explicitly labelled approximate. Closed, oriented input topology
is still required. Geometric defects can affect the estimates. This is not a
general solution for holes, arbitrary face reversals, or invalid solid regions.
Those inputs can still use the existing winding-grid method, whose dense-grid
limit and performance have not changed. The fast method does not simplify,
repair, or increase the tolerance of the input meshes.

The render check also found that publishing a different mesh origin displaced
the display, even when world-coordinate exports and volumes were correct.
Both approximate methods now restore their vertices to input A's coordinate
frame before publication. They retain A's origin for display and picking.
Unit tests check this contract at large scene origins.

The previous exact-output requirement was stronger than the library's geometric
contract. Manifold distinguishes exact topology from geometry that is valid
within numerical precision. See its
[epsilon-validity definition](https://github.com/elalish/manifold/wiki/Manifold-Library#definition-of-%CE%B5-valid).
Its status alone is not an exact-intersection certificate.

A separate synthetic example reproduces the blocker with two rotated boxes,
each with only 12 triangles. A 0.1 displacement gives an expected difference of
2 cubic units on each side. Direct mesh rejects the output; Approximate mesh
returns those volumes. Tests also cover displacements of `1e-6` and `1e-9`,
large scene origins, cavities, internal unit boundary movement, unchanged source
data, invalid topology, cancellation, and scene/CLI persistence.

The unit mapping remains inferred from geometry. Separate unit comparisons
retain changes at internal boundaries. A region transferred between two units
appears as removal from one and addition to the other, so summing all per-unit
changed volumes counts that region twice.

### Final real-data result

All nine matched pairs completed with the production implementation. The
isolated Release run took 16.989 seconds in total, with 0.137 to 3.974 seconds
per pair. These times include input reading inside `buildDifferenceMesh`, both
Boolean operations, output checks, and mesh publication. They exclude file
loading and comparison input assembly. No other build, viewer, or test from
this task ran during that capture. This is one local timing run, not a timing
guarantee or a direct speedup ratio against the earlier whole-model test.

The small Rowan pair completed in 0.137 seconds. A-only volume is 23,829.699 and
B-only volume is 14,641.081 model units cubed. Mulbring completed in 0.173 seconds,
with A-only volume 14,826.108 and B-only volume 6,384.155. These are approximate
estimates, not independently established ground truth. The complete nine-pair
records are in
[`approximate-isolated-pairs.jsonl`](benchmarks/gm-difference-20261018/approximate-mesh/approximate-isolated-pairs.jsonl).

Open `D:/temp/GM-data/Open-unit-approximate.cmd` to load
`D:/temp/GM-data/test-unit-approximate.woby` with the local Release executable and
both importers. All nine comparisons select the fast method. The small Rowan
comparison is created and visible; the other eight are ready for creation when
selected. The original scene and the earlier comparison copy are unchanged.
The new copy was reopened in the application. Its Rowan result was ready, its
volumes matched the probe, and its geometry rendered after framing the result.
See the [render capture](benchmarks/gm-difference-20261018/approximate-mesh/approximate-final-rowan.png).

The final full Release build passed without compiler warnings. All 91 tests in
the default CTest selection passed in 36.89 seconds. This selection excludes
slow and graphics tests. The separate difference CLI test passed in 71.79 seconds.
It checks creation, export, scene reload, cancellation, and the existing grid method. The final
source and executable hashes, compact numerical experiments, reproduction
sources, and logs are under
[`approximate-mesh`](benchmarks/gm-difference-20261018/approximate-mesh).

## Earlier follow-up: steps one and two

The user authorized the geometric mapping. This does not confirm the geological
names in PLAXIS. Each new comparison has the suffix `[geometric match]`.

### Scene copy

Created `D:/temp/GM-data/test-unit-comparisons.woby` through the scene operations
and save API. The original scene is unchanged; its SHA-256 is
`2bdbfd97611a65a4ac2da3e4a1e3357c33b067c82d06967e4da1090170658d41`.

The copy contains the nine separate pairs listed below. Each comparison has one
unit on each side. It excludes the external void, uses scene coordinates, and
locks identity placements. The old +24.080139 PLAXIS Z offset is absent.
The small Rowan pair is visible in overlay mode. The other comparisons and the
source-model displays are hidden. Automatic detectors are off. Direct mode is
selected, but difference creation is not started. This prevents repeated failed
calculations when the scene opens.

`D:/temp/GM-data/Open-unit-comparisons.cmd` opens the copy with the local Release
build and both importers. It depends on
`D:/woby-builds/gm-difference-20261018/bin/Release/woby.exe`.
The launcher does not need persistent importer registration.

The copy was reopened in the application. All nine analyses were present, the
Rowan overlay rendered, and surface-distance results completed. A to B has mean
distance 0.26284, 95th percentile 0.48192, and maximum 81.20777 (8,136 samples).
B to A has mean 0.11457, 95th percentile 0.47008, and maximum 5.05956 (6,368
samples). These are sampled surface distances in model units, not volume
measurements or maximum-distance bounds. The large outliers must not be hidden
by the smaller percentile values.

### Confirmed conversion defect and fix

The previous Direct output conversion merged every pair of equal-coordinate
vertices. This can join separate closed components that touch at an edge and
create a non-manifold edge. Manifold supplies explicit property-seam merge
indices; equal coordinates do not establish shared topology.

`src/difference_mesh.cpp` now uses those explicit indices and preserves all
other vertex identities. Output with no property seams needs no extra remap
buffers. The output intersection check identifies exact geometric vertex/edge
contacts separately from the topology check. It still rejects overlaps beyond
those contacts. Input validation remains strict. Output errors now identify
`Output A only` or `Output B only`.

A synthetic regression subtracts the lower half of a notched prism. Its two
remaining triangular prisms touch at one edge and have a known total volume of
1. Before the fix, it threw the same non-manifold error as the supplied data.
After the fix, both subtraction orders pass, preserve separate contact vertices,
and have closed, consistently oriented edges. Another check confirms that
interpenetrating input solids are still rejected. These tests use no private
model data.

### Remaining numerical failure

The fix does **not** make either real pair produce an accepted Direct mesh.
Both Rowan and Mulbring now pass the output topology check but fail its exact
self-intersection check. The application also reports this output-stage failure
for the Rowan pair in the new scene.

Final native probes reached this rejection in 0.100 seconds for Rowan and
0.165 seconds for Mulbring, excluding scene load. These are rejection times,
not successful mesh construction times.

For Rowan A only, Manifold reports `NoError`, 3,174 vertices, 6,388 triangles,
zero property-seam merges, and epsilon/tolerance `3.1955252884328364e-9`.
The first intersection is between output triangles 44 and 61. Triangle 61 has
longest edge `2.6414677208260234e-8` and area `1.5861016049625933e-17`.

Exact rational arithmetic on the stored doubles confirms the intersection.
The two non-shared vertices of triangle 61 lie on opposite sides of triangle
44's plane, at signed distances approximately `-4.18614e-14` and `3.71454e-15`.
Their connecting edge crosses strictly inside triangle 44, about `6.07707e-9`
from the shared vertex. This is not only an equal-coordinate contact or a false
positive in the detector. Its scale is consistent with Boolean rounding error,
but this does not prove that every intersection in either output is negligible.

An isolated experiment with Manifold's default `Simplify()` still failed exact
intersection validation for both pairs. That experiment was not added to the
production code. At that stage, no intersection epsilon, grid-limit increase,
or validation bypass was added. The later fast approximate method is described
above. The approximate grid and winding costs remain unresolved.

The initial next step was a strict numerical output contract and possible
bounded correction before exact validation. That direction was superseded by
the user's request for an approximate result without mesh repair. The later
investigation captured all output intersections and reduced a complete Boolean
input example to two boxes. A small first finding alone would not justify an
exact-validity claim. Larger-unit validation budgets remain a separate task for
Direct mesh.

Follow-up measurements, exact witness calculations, scene audit, probe sources,
and validation logs are in
[`benchmarks/gm-difference-20261018/followup`](benchmarks/gm-difference-20261018/followup).
The final full Release build succeeded without compiler warnings. All 91 tests
in the default CTest selection passed in 33.66 seconds. This selection excludes
slow and graphics tests, as specified by the repository preset. The targeted
difference run also passed all 24 cases, including the new regression. The
inspection viewer was closed after the scene checks.
The sections below retain the initial investigation and its baseline timings.

## Scene and importers

The scene opened successfully with both libraries from
`D:/temp/GM-data/importers`:

- `geosim_parquet/woby_geosim_parquet_importer.dll`: GeoSim / Evo, version 4.1.0.
- `plaxis_sqlite/woby_plaxis_sqlite_importer.dll`: PLAXIS SQLite, version 1.0.0.

The original scene and data files were not changed. The inspection instance was
closed. Importers were loaded for the investigation, without a persistent
registration.

| Input | Enabled groups | Triangles |
| --- | ---: | ---: |
| A, `original/object.json`, named `input` | 10 | 1,161,720 |
| B, `plaxis/data.db`, named `plaxis` | 9 | 108,632 |

The third loaded file, `parametric/object.json`, is not a comparison input.
It has 197,337 triangles. Hidden source files still contribute to a comparison
when their comparison members are enabled.

The saved comparison uses Approximate volume, cell size 5, origin alignment,
and locked placement. Its acceptance tolerance is about 22.63 and its heatmap
range is about 27.98. These last two settings do not control difference geometry.

## Placement and unit identity

Every saved B placement contains a Z translation of **+24.08013916015625**.
A placements have zero translation. The file centres used by the alignment are
Z = 160.932266 for input and Z = 136.852127 for PLAXIS. Their difference explains
the saved offset. This offset is separate from the comparison's display position.

The raw source bounds already agree closely in X and Y. Their geological
surfaces are much closer in Z before this offset. For a comparison in the source
coordinate system, unlock placement, select scene alignment, and then lock the
placement again if required. Changing alignment while placement is locked is
rejected by the operation layer.

Input A also includes `External`: 531,694 triangles, or about 45.8% of A.
Its source metadata declares `feature = "Void"` and an external material key.
It includes outer faces and reversed copies of faces shared with geological
volumes. Its signed triangle volume is approximately 3.413 billion model units
cubed. It is not another soil unit. The saved request therefore compares soil
plus external void on A with soil on B.

The difference implementation builds one triangle soup and one winding field
for each side. It does not retain a material label in the Boolean calculation.
Oppositely oriented shared interfaces cancel in the field. A moved internal
boundary can therefore produce no whole-model solid difference when the outside
shape stays fixed. The `per_volume` file setting does not turn this difference
request into separate unit comparisons.

Use corresponding geological units as separate inputs. Exclude `External` from
soil comparisons. A later combined result must retain unit labels. Adding all
per-unit A-only and B-only volumes can count the same physical region twice when
its material changes.

## Grid limit

The bounds used by the saved request span approximately
**3267.565 × 5637.436 × 321.865** model units. The code counts both BCC lattices
and rounds each index dimension up to a power of two. Its guard is:

```text
size[k] = ceil(extent[k] / cellSize) + 4
axisSlots[k] = 2 ^ (floor(log2(size[k] + 3)) + 1)
guardCount = 2 * axisSlots[0] * axisSlots[1] * axisSlots[2]
reject if guardCount > 8,000,000
```

| Cell size | Guard count | Allocation check |
| ---: | ---: | --- |
| 0.5 | 274,877,906,944 | Rejected |
| 1 | 34,359,738,368 | Rejected |
| 5, saved value | 536,870,912 | Rejected |
| 10 | 67,108,864 | Rejected |
| 20 | 8,388,608 | Rejected |
| 25 | 4,194,304 | Accepted |
| 50 | 524,288 | Accepted |
| 100 | 262,144 | Accepted |

For these bounds the minimum accepted cell size is approximately
**22.731596**. The power-of-two rounding causes abrupt jumps. In particular,
8,388,608 exceeds the decimal eight-million limit.

The guard count is a conservative padded count, not the number of calls to the
winding function. At cell size 5, the installed Manifold implementation would
address 346,845,073 doubles in its voxel buffer, about 2.58 GiB for that buffer
alone. It would call the field about 105.4 million times per side in the initial
sampling pass. Extraction and output require more work and memory.

The source metadata specifies a minimum layer thickness of 0.5 m. Even the
minimum accepted whole-model cell size is far larger than this detail. Smaller
physical differences do not cause the allocation error directly: a smaller
sampling cell over the full model bounds causes it. Raising the limit does not
remove the sampling cost or preserve material identity.

## Measurements

The probes use production loading, scene restoration, comparison snapshots,
world-mesh assembly, and `buildDifferenceMesh`. They load the actual scene and
the supplied importers. Timed difference calls exclude file loading and world
assembly. Settings changes for pair probes use `ui_operations`; the original
scene remains unchanged.

Build: commit `679ca03`, MSVC 19.51.36243, Release, `vs2026-vcpkg`.
The installed Manifold is 3.5.2 with the repository's LevelSet allocation patch
and `MANIFOLD_PAR=OFF`. The approximate path also explicitly requests sequential
LevelSet callbacks. The machine is the same Ryzen 7 5800H used in the earlier
report. These are individual desktop runs, not repeated benchmark medians.

| Request | Difference time | Result |
| --- | ---: | --- |
| Saved whole model, approximate, cell 5 | 1.161 s | Grid-limit error |
| Saved whole model, direct | 1.150 s | Input A topology error |
| Saved whole model, approximate, cell 100 | >60 s | Canceled at 60.024 s; no result |
| Rowan Formation `__2` / Soil_4_1, direct, scene alignment | 0.130 s | Output topology error |
| Mulbring Siltstone / Soil_1_1, direct, scene alignment | 0.229 s | Topology error |
| Rowan Formation Pgr5 / Soil_3_1, direct, scene alignment | 2.704 s | Input A intersection-validation work limit |
| Rowan Formation `__2` / Soil_4_1, approximate, cell 10, scene alignment | 12.361 s | Completed |
| Same pair, approximate, cell 5 | >60 s | Canceled at 60.023 s; no result |

The successful pair has 2,034 input triangles and 1,592 PLAXIS triangles.
At cell size 10 it produces 1,940 A-only triangles and 1,128 B-only triangles.
Its approximate volumes are 21,666.667 A-only, 12,500 B-only, and 34,166.667 total
model units cubed. These are coarse-grid results. The finer run did not complete,
so there is no resolution-convergence evidence for those volumes.

### Where the approximate calculation spends time

A separate instrumented whole-model run at cell size 100 was stopped after
60.020 seconds:

- Reading and exact-position welding: about 1.17 seconds in total.
- Building the input A winding tree: 1.502 seconds.
- Input A LevelSet sampling: 57.327 seconds, about 95.5% of elapsed time.
- PLAXIS reconstruction and Boolean subtraction: not reached.

All of the first **502 attempted field evaluations** entered the full exact
solid-angle fallback. The last evaluation was interrupted by cancellation.
The first sampled query evaluated 1,165,629 triangle solid angles, including its
initial tree estimate and fallback. A full fallback visits the approximately
1.16-million-triangle input. The sampled query is one point, not an average
over the whole grid. The 502-point prefix also must not be treated as the
fallback rate for the complete field.

The winding tree accepts a distant node only when its distance exceeds four
node radii. It adds conservative error bounds across nodes. If the total bound
cannot decide the inside/outside threshold, `inside()` starts a second traversal
with approximation disabled. Opposed shared faces can cancel their signed
contributions while their positive error bounds remain large. This is a
plausible cause of the fallback rate; a separate optimization experiment is
needed to quantify the benefit of removing such pairs.

Process CPU use during the last 30 seconds was about 0.99 logical CPUs. The delay
is active serial CPU work, not a wait for the renderer. A and B are reconstructed
in sequence. Rebuilds and resolution checks construct their soups and winding
trees again.

Two initial cell-size-500 captures overlapped for part of their duration. They
are retained in the evidence but excluded from the timing table. The native and
instrumented cell-size-100 runs above ran separately, without our build or tests
running.

### Direct mesh requires a separate fix

The instrumented Rowan `__2` pair passes input A and input B validation, then
constructs both solids. The failure occurs when validating the first Boolean
output. Thus, its message about input topology is not a correct location for
that failure.

The direct output path joins every equal-position vertex before validation.
In the Rowan pair, Manifold reports `NoError` and returns 6,388 triangles with
3,174 vertices. Woby reduces that to 3,128 vertices, then rejects the topology.
This is evidence that the output conversion needs investigation; it is not
proof that bypassing validation would give a correct result.

Joining these vertices can join separate topological components at a contact. The approximate
output path already preserves these vertices separately. Investigate the direct
output conversion and contact validation before recommending direct mode for
these pairs. Do not remove the validation checks to make the result pass.

## Candidate unit mapping

This mapping is inferred from spatial bounds and signed triangle volumes. It is
not a material-name mapping supplied by PLAXIS. The database contains only
`NULL MATERIAL` in `Material`; its staged soil features have generic soil names.
The user authorized its use for this investigation. It remains an inferred
mapping and does not establish authoritative geological identity.

The minimum cell sizes below use each candidate pair's full bounds with scene
alignment. They pass the allocation check only; they do not establish accuracy
or acceptable running time.

| Input geological unit | PLAXIS group | Minimum cell size |
| --- | --- | ---: |
| Unknown | Soil_8_1 | 9.348 |
| Pdag-Gyarran Volcanics | Soil_7_1 | 20.981 |
| Pgs-Skeletar Formation | Soil_9_1 | 17.373 |
| Pmtb-Branxton Formation `__1` | Soil_2_1 | 11.948 |
| Pgr5-Rowan Formation `__1` | Soil_3_1 | 14.796 |
| Pgr3-Rowan Formation `__1` | Soil_5_1 | 13.655 |
| Pgr3-Rowan Formation `__2` | Soil_4_1 | 3.177 |
| Pgr1-Rowan Formation | Soil_6_1 | 13.176 |
| Pmtm-Mulbring Siltstone | Soil_1_1 | 2.761 |

Splitting by unit reduces work, but the dense-grid limit still prevents
sub-metre resolution for all nine full unit pairs.

## Relation to the earlier report

[`detector-performance-analysis-20261017.md`](detector-performance-analysis-20261017.md)
is present in this checkout. It measured the direct difference path at commit
`61efba0`, with Manifold 3.4.1. Its difference times are not measurements of this
approximate winding-grid path.

Useful earlier results include 7.264 seconds for a successful gap between
131,072-triangle sheets, with about 91% spent in validation. Both generated
families reached a validation limit at 524,288 triangles per side. Those results
support work on direct validation. They do not explain the present grid error
or the measured full exact winding fallbacks.

## Initial recommended changes

1. Make the requested comparison explicit: matched geological units in scene
   coordinates, without the external void. Preserve unit identities in any
   combined difference display. Use per-unit surface distances for inspection
   while a reliable fine-resolution volume method is unavailable.
2. Investigate direct Boolean output conversion and contact validation on the
   small Rowan and Mulbring pairs. Improve the error to identify input versus
   output failure. Address the larger-pair validation budget separately.
3. Add a grid preflight that reports the requested count and minimum accepted
   cell size. Show calculation stages and progress. Do not offer a larger cell
   as if it guarantees adequate resolution or a fast result.
4. Measure winding improvements with the same complete inputs: refine only the
   nodes responsible for an ambiguous bound, retain owned trees between
   resolution changes, and test removal of exactly opposed coincident faces
   within each label. Preserve orientation, cavity, and cancellation behavior.
   Parallel sampling needs bounded memory and safe cancellation handling.
5. For small differences across this domain, evaluate a sparse or adaptive grid,
   or a robust direct method. A dense full-domain grid remains unsuitable even
   after faster winding queries. Any tiled method must preserve thin features
   and join its tile boundaries correctly.

## Evidence and reproduction

Portable evidence is in
[`benchmarks/gm-difference-20261018`](benchmarks/gm-difference-20261018).
It includes result records, scene/group bounds, stage timings, CPU samples,
grid calculations, candidate mappings, and source/data/executable hashes.
The retained probe source is the final investigation snapshot. Earlier binaries
were replaced as pair selection and output counters were added; each capture
retains its own executable hash. Instrumented timings include probe overhead.
Large binaries and the local probe build remain at
`D:/woby-builds/gm-difference-20261018`.

The probe loads the full scene with its saved settings. The `pair` and
`pair-direct` modes additionally select zero-based A/B member indices, unlock
placement, and use scene alignment. For example:

```powershell
D:/woby-builds/gm-difference-20261018/bin/Release/gm_difference_probe.exe `
  D:/temp/GM-data/test.woby D:/temp/GM-data/importers 5 30 full
D:/woby-builds/gm-difference-20261018/bin/Release/gm_difference_profile.exe `
  D:/temp/GM-data/test.woby D:/temp/GM-data/importers 100 60 full
D:/woby-builds/gm-difference-20261018/bin/Release/gm_difference_probe.exe `
  D:/temp/GM-data/test.woby D:/temp/GM-data/importers 10 60 pair 6 3
```

The numeric limit argument requests cooperative cancellation after that many
seconds of difference computation. It is not a completion-time measurement for
canceled work. The runner also has an external timeout. Production algorithms
were not changed during those initial captures. The follow-up above records the
later output-conversion change and its limits.

Validation: the complete Release build succeeded without compiler warnings.
The default CTest selection passed all 91 tests, including the difference-mesh
tests. As in the repository's default test preset, slow and graphics tests were
excluded. No document-content tests were added.
