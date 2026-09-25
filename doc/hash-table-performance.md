# Hash tables and scene query performance

Measured on 25–26 September 2026 on Windows, Ryzen 7 5800H (8 cores / 16 threads),
with Visual Studio 2026, MSVC 14.51.36231, Boost.Unordered 1.90.0 and
unordered_dense 4.8.1 from vcpkg. All timings use Release builds. Baseline
production sources are commit `dc1508b4ffa54228ed3dc54c4389619ad14e437d`, with
the new benchmark driver linked in. No builds, tests or other Woby processes ran
alongside the measurements. These measurements are workload-specific, not claims
about every use of the containers or application frame times.

## Implemented changes

- Use Boost flat maps/sets for transient scene lookups, property target
  deduplication, saved-view references and comparison traversal membership.
  Reserve capacity when the input size is known.
- Resolve large property target lists in one scene traversal instead of scanning
  all parts separately for each target. Large explicit selections also resolve
  object IDs through a temporary index. Small selections retain direct lookup.
- Index pointers to saved-view records instead of copying large settings records
  into the hash table. The source vector stays alive and unchanged throughout
  the lookup operation.
- Preserve selection order through the target vector. Comparison traversal order
  still comes from the scene tree; the visited set is only a membership filter.
  No output order depends on flat-container iteration order, and no element
  references survive an insertion that could rehash the container.

The remaining `std::unordered_map` in `main.cpp` matches filesystem paths to
completed import outcomes. It is reserved and used once per batch, not per frame.
Ordered containers in detector reporting retain their deterministic ordering.

## Container measurements

The microbenchmark builds, queries and destroys each transient table. ID sets
include duplicate insertion, successful lookup and unsuccessful lookup; ID maps
store the same `pair<int,int>` used by scene-document part locations. Keys are
shuffled, unique, sequential IDs with a fixed seed. Part membership keys are
two-component indices. All tables are reserved except the explicitly unreserved
standard set. Geometry probes use the same `analysisKeyHash` for every container
and four insertion/lookup passes with insertion-order IDs.

The Woby candidate reuses `AnalysisIndex`, with one-element keys for IDs and a
parallel value vector for maps. The benchmark supplies a read-only lookup because
the production index only exposes insert-or-find. The part and point comparisons
share the custom hasher; scalar library containers use their default hashers.

Median microseconds per complete table lifecycle at 4,096 entries, seven timed
batches after warm-up:

| Workload | Standard | Standard reserved | Woby custom | Boost flat | unordered_dense |
| --- | ---: | ---: | ---: | ---: | ---: |
| ID set | 771.51 | 714.80 | 236.11 | **114.07** | 160.01 |
| ID map | 655.99 | — | 140.38 | 102.21 | **99.91** |
| Part membership set | 2,722.70 (ordered set) | — | 253.29 | **172.14** | 453.08 |

At 524,288 coordinate keys, the standalone point-index medians were 543.37 ms
for the standard map, 267.05 ms for Woby, 259.79 ms for Boost and 227.41 ms for
unordered_dense. That isolated result does not establish the same improvement
in complete detector workloads.

Boost is the production scene-container choice: strong results across these
small and medium integer workloads, close to unordered_dense for maps, and it
extends an existing Boost dependency. unordered_dense is only required by the
optional `hash-benchmarks` vcpkg feature.

Raw measurements for every tested size: [container CSV](benchmarks/hash-containers.csv).

## Actual scene functions

Synthetic scenes contain one selected file with 16, 256 or 4,096 parts, an analysis
including all parts, and two saved views. Mesh nodes reference a small triangle;
this measures part-count scaling rather than geometry throughput. Setup is outside
the timer. The inspector workload queries all 15 properties once, the document
workload creates and destroys a complete `SceneDocument`, and the signature
workload calls `comparisonGeometrySignature`. Each reports a median of seven
timed batches, with a warm-up call.

| Workload, 4,096 parts | Baseline | Container swap only | Final | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Inspector property queries | 62.83 ms | 60.59 ms | **2.17 ms** | **28.9x** |
| Document snapshot | 8.78 ms | 5.98 ms | **5.58 ms** | **1.58x** |
| Comparison geometry signature | 3.13 ms | 2.22 ms | **2.22 ms** | **1.41x** |

Most of the inspector gain comes from eliminating repeated scene scans; replacing
the set alone barely affects the largest case. At 256 parts the initial document
timings were 0.476 ms before and 0.500 ms after, so the data does not demonstrate a
win at every size. At 16 parts they were 0.0212 ms and 0.0171 ms.

A second before/after run measured 62.15 → 2.01 ms for the 4,096-part inspector,
6.97 → 5.83 ms for the document and 3.11 → 2.23 ms for the signature. The inspector
improvement held at about 29–31x and the signature at about 1.4x; the document
improvement varied between 1.2x and 1.6x. Repeat data:
[before](benchmarks/hash-scene-before-repeat.csv),
[after](benchmarks/hash-scene-after-repeat.csv).

Raw data: [before](benchmarks/hash-scene-before.csv),
[container-only](benchmarks/hash-scene-boost.csv),
[final](benchmarks/hash-scene-after.csv). Property and document checksums match.
Geometry signatures include allocation addresses, so their numeric values are
only comparable within one process; each timed batch verifies stability. Unit
tests separately check signature stability across repeated and implicit trees.

This benchmark does not include ImGui drawing, selected-object identity rows,
property mutation, GPU work or frame scheduling. Large property edits and other
scene scans can still be separate scaling limits. The current eight-target
cutoff is a conservative implementation choice, not an exhaustively tuned value.

## Complete detector comparison

The same 1,054,542-triangle, 535,893-point, 65-part OBJ from
[automatic detector performance](detector-performance.md) was loaded and expanded
to match the application's triangle-corner input. Only the `AnalysisIndex` lookup
backend differs between executables. These timings cover all nine automatic
detectors (stage 78), excluding OBJ load, source copy, rendering and GPU upload.

| Backend | First run: median of 5 | Reverse-order repeat: median of 7 | Repeat range |
| --- | ---: | ---: | ---: |
| Woby custom | 974.27 ms | 945.81 ms | 801.92–1,016.88 ms |
| Boost flat map adapter | 886.65 ms | 902.07 ms | 807.67–1,013.52 ms |
| unordered_dense map adapter | 836.22 ms | 886.58 ms | 796.05–994.75 ms |

The first order was Woby, Boost, unordered_dense; the second was the reverse.
Every run returned the same nine detector counts. These prototypes show modest
median improvements (roughly 5–14%), with wide overlapping ranges and added key
storage. They do not justify replacing the specialized geometry index in this
change. The production `AnalysisIndex` and OBJ `VertexIndexTable` remain intact;
the latter was not substituted in the full-detector comparison. Keep the probes
for future evaluation on more models, including memory measurements.

Raw stage timings and counts: [detector CSV](benchmarks/hash-detectors.csv).

## Reproduce

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON -DWOBY_BUILD_HASH_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_hash_benchmarks woby_tests woby_benchmarks woby_benchmarks_boost woby_benchmarks_ankerl
build/vs2026-vcpkg/bin/Release/woby_hash_benchmarks.exe
build/vs2026-vcpkg/bin/Release/woby_tests.exe '--test-case=scene query benchmark' --no-skip=true
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe detectors-expanded C:/work/temp/uploads_files_2720101_BusGameMap.obj 7
build/vs2026-vcpkg/bin/Release/woby_benchmarks_boost.exe detectors-expanded C:/work/temp/uploads_files_2720101_BusGameMap.obj 7
build/vs2026-vcpkg/bin/Release/woby_benchmarks_ankerl.exe detectors-expanded C:/work/temp/uploads_files_2720101_BusGameMap.obj 7
```

The scene benchmark is explicitly skipped during normal test execution and has
no timing assertions. Geometry comparison executables use generated copies of
the production detector sources with a benchmark-only index adapter. Production
source files are not rewritten. All three preserve first-use IDs, coordinate
equality, signed-zero handling and the key-vector interface. The library adapters
also store keys in their map, so they have extra key storage versus Woby's
index-only buckets; memory consumption was not measured.

Use the same driver on both revisions for before/after comparisons. Retain the
baseline executable beside its runtime DLLs before rebuilding the changed
sources. Run measurements sequentially, without a viewer, build or tests in the
background. Neither CPU thermal state nor OS scheduling is controlled.

## Validation

The Debug build and the Release benchmark builds completed without compiler
warnings. All **606 CTest entries passed**, including the four slow and two
graphics tests. New regression coverage checks large selections, first-target
display values, mixed values, overlapping selections, missing-target rejection,
fresh results after edits, folder aggregation and comparison signature stability.
