# Production vertex lookup and source-coordinate storage

Implemented against `1d7f1ad` after the
[vertex-mapping investigation](vertex-mapping-performance.md).

A [follow-up mapping investigation](vertex-mapping-detail-2026-09-28.md)
measures actual lookup frequencies, growth costs, and separate serial/parallel
attribute-gathering experiments against the current implementation.

## Production changes

The OBJ loader now uses a uint32 primary entry per source position. A position's
first normal/UV tuple goes directly into that entry; alternate tuples use a
secondary hash table. The table stores global first-use render IDs across
shapes. No value-based compaction is added: different source attribute indices,
UV seams, hard normals, triangle order, and source identities are preserved.

If both normal and UV attribute arrays are empty, the loader uses only the
position-to-render-ID array. It does not allocate tuple keys or hash buckets.
Every file allocates the primary array for all source positions, including
unused positions. There is no sparse-file fallback. Position-only files always
use direct lookup; files with attribute arrays hash only alternate tuples for
each position.

`SourceMeshData::points` now stores `std::array<float, 3>`: **12 bytes per
point instead of 24**. OBJ and plugin import paths already supply float
coordinates, so storing their values as doubles had added no input precision.
All source positions, including unused OBJ positions, and original triangle
indices remain available to diagnostics. GPU vertex/index layouts are unchanged.

Analysis promotes points before arithmetic through `promoteSourcePoint`:

- Duplicate grouping compares exact float source coordinates, including the
  existing signed-zero equality rule. Display transforms promote before
  multiplying or adding, then convert the display result to float.
- Degenerate-triangle inspection promotes the three points it is processing,
  then performs world transforms and classification in doubles.
- Topology construction promotes before world transforms and retains double
  positions in its derived analysis cache. Intersection predicates continue to
  use those doubles and exact-rational fallbacks.

There is no full double source-array allocation on the first analysis.
Double computation and derived double storage occur where an analysis needs
them. The double-precision triangle predicate APIs remain unchanged. The source
container itself now accepts float coordinates only; no old double-storage
compatibility path is retained.

## Regression coverage

New tests check global first-use IDs across groups, unused source positions,
primary lookup with many unused positions, position-only data, normal-bearing data, and unused
attribute arrays. Existing OBJ tests cover seam-table growth, negative indices,
UV flipping, polygon triangulation, missing normals, and Unicode filenames.

Precision regressions exercise:

- A unit triangle translated by 2^24, which would collapse if transformed in
  float arithmetic before promotion.
- Duplicate display coordinates calculated as `2^24 + 1 - 2^24`, which must
  yield 1 rather than 0.
- Tiny source coordinates and transforms whose product underflows floats but
  whose topology and fin areas remain valid in double precision.
- Exact duplicate equality and distinct adjacent representable source floats.

Synthetic source fixtures now explicitly convert coordinates to the float
source representation. Tests of double-precision mathematical predicates
remain in double precision. The former source fixture using coordinates near
1e-200 was replaced by a tiny-float-source/transform test matching the new
source contract.

## Benchmark method

The optional `woby_mapping_adaptive` executable now instruments the production
lookup directly. `hybrid` disables just the position-only shortcut, and
`baseline` substitutes a frozen historical tuple table for experimental
comparisons. All three use the current float source representation; the
application has no selector for the historical implementation.

Source-coordinate fingerprints use a canonical double encoding after timing,
promoting one point at a time. This allows exact comparison against the previous
double-storage experiment while recording the new actual source allocation
size. Render vertices, both index streams, node identities, point ranges,
bounds, and radius must match the historical records too.

## Results

Three fresh Release processes per model, run serially after build and test work
finished. All **15/15 runs** matched the historical geometry fingerprints,
counts, bounds, and radius. Each source-coordinate allocation is exactly half
its previous size. [Raw runs and summary](vertex-mapping-production-results.json)
include timing ranges and memory counters.

| Model | CPU load, historical → current (s) | Current mapping (s) | Source coordinate saving (MiB) | Load peak resident memory, historical → current (MiB) |
|---|---:|---:|---:|---:|
| BearTrap | 29.638 → 19.406 | 9.425 | 433.6 | 9997.3 → 9457.0 |
| Bennu | 6.020 → 2.952 | 1.082 | 102.2 | 1926.4 → 1691.9 |
| Powerplant | 4.333 → 3.861 | 2.446 | 68.5 | 1722.6 → 1646.2 |
| San Miguel | 3.865 → 3.227 | 1.753 | 67.9 | 1680.5 → 1587.2 |
| BusGameMap | 0.327 → 0.213 | 0.070 | 6.1 | 140.8 → 132.7 |

Timings and peak resident memory are medians. Mapping includes allocation and
the conversion loop. Coordinate savings are exact array-size differences, not
sampled process-memory deltas. The reference measurements came from the earlier
same-machine experiment; this is not an interleaved A/B sweep. File I/O and
system conditions vary, so the measured mapping reductions are a more direct
comparison than total load differences. GPU payload and rendering are unchanged.
These timings precede removal of the sparse-file fallback; none of the five
measured models selected that fallback.

## Validation

- Debug application and enabled research targets built without warnings using
  the `vs2026-vcpkg` preset. Release research targets also built without warnings.
- After removing the sparse-file fallback, the Debug build passed without
  warnings and the full suite passed **632/632**, including slow and graphics
  tests, in 227.52 seconds (`build/remove-mapping-fallback-tests.log`).
- The mapping contract requires a primary entry for every source position,
  including unused positions, and no tuple-key or hash-bucket allocations for
  position-only files.
- Research contract checks passed in Debug and Release, including single-variant
  sweep selection. No document-content tests were added.

## Reproduce

```powershell
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure
cmake --build --preset vs2026-vcpkg --config Release --target woby_mapping_baseline woby_mapping_hybrid woby_mapping_adaptive
uv run tests/vertex_mapping/contract_test.py build/vs2026-vcpkg/bin/Release
uv run tests/vertex_mapping/run.py --bin build/vs2026-vcpkg/bin/Release --models D:/temp/obj_tests --output build/vertex-mapping-production.jsonl --rounds 3 --variants adaptive
```
