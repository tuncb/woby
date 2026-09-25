# Automatic detector performance

Measured on 25 September 2026 on Windows, Ryzen 7 5800H (8 cores / 16 threads),
using MSVC Release builds from the `vs2026-vcpkg` preset.

The input is `C:\work\temp\uploads_files_2720101_BusGameMap.obj`: 1,054,542
triangles, 535,893 source points, and 65 parts. Each repetition computes fresh
results for one populated side and all nine default automatic detectors.
Self-intersections remain manual by default. Detection thresholds and source
provenance rules are unchanged.

## Measurements

| Workload | Before | After |
| --- | ---: | ---: |
| Compact input, automatic detectors plus source copy, median of 3 | 15,158 ms | 881 ms |
| Expanded application input, detectors only, 5 runs | Not measured | 834–954 ms |
| Expanded application input, detectors plus source copy, 5 runs | Not measured | 870–996 ms |

The compact-input baseline used an empty cancellation token. Updated runs use a
live cancellation source, matching the application. Empty tokens concealed an
additional application bottleneck: passing the token by value inside every loop
caused atomic reference-count contention across detector workers. Loop helpers
now take the token by reference, while still checking cancellation.

CPU measurements exclude OBJ loading, world-space snapshot assembly, GPU upload,
and application/CLI scheduling. A separate headless application check confirmed
all nine detectors complete with the same counts. Three fresh analysis creations,
timed through the first successful CLI findings response, took 1.34–1.67 seconds.
Those measurements include the additional preparation and command round trips;
the entire create-analysis/query sequence remains above one second. Timings
depend on CPU load and hardware, and are not unit-test assertions.

Retained counts are 17,058 boundary edges, 510 duplicate points (413 groups), 270
degenerate triangles, 4 non-manifold vertices, 22 holes, and 21 fins. Non-manifold
edges, inconsistent winding, and duplicate triangles all remain zero.

## Changes

- Dense insertion-ordered hash tables replace allocating tree lookups. Signed
  zero remains equivalent and unequal finite coordinates remain distinct.
- Integer vertex buckets replace global edge and triangle sorts. Finding order,
  source IDs, incident-face order, and winding information are retained.
- Topology incidence lives in contiguous immutable storage. Copied snapshots
  share ownership so vertex and edge spans remain valid after the original dies.
- Vertex links reuse scratch storage; manifold fin checks reuse established
  components. Consistent orientation constraints avoid a redundant traversal.
- A conservative floating-point filter rejects clearly non-collapsed triangles;
  ambiguous, underflowing, and overflowing cases retain interval/exact arithmetic.
- Requested automatic detectors run as a bounded parallel batch. Degenerate
  classification also uses the shared worker budget. Small inputs stay serial.
- Editing thresholds or topology mode retries remaining batch participants.
  Explicit cancellation stays canceled. Failed batches retry stages separately
  to keep an unrelated detector usable.
- Detector computation can overlap GPU staging of the preceding source result.

## Reproduce

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_benchmarks
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe detectors C:\work\temp\uploads_files_2720101_BusGameMap.obj 3
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe detectors-expanded C:\work\temp\uploads_files_2720101_BusGameMap.obj 3
```

Stage `1` is the source copy; stage `78` is the automatic detector batch.
The expanded workload mirrors the flat triangle-corner geometry assembled by
`comparisonWorldMesh`, retaining the same source-data provenance. Run benchmarks
without another build, test suite, viewer, or benchmark competing for resources.

Validation includes Debug and Release builds with no compiler warnings, the full
CTest suite (600 tests including slow and graphics tests), and targeted coverage for hash
growth, signed zero, welded provenance, copied snapshot lifetimes, overlapping
instances, batch cancellation/recovery, and exact rational predicate oracles.
The final full run passed 599/600; the unrelated updater smoke test encountered
a temporary metadata-commit failure and passed when rerun in isolation. An
earlier full run also passed all 600 tests.
