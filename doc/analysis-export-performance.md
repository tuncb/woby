# Diagnostic JSON export performance

Issue [#108](https://github.com/tuncb/woby/issues/108), measured 2026-10-04.

The full exporter now writes finding records directly into a reusable 64 KiB
buffer instead of constructing a JSON tree for each record. Integer fields use
`to_chars`; the existing JSON library still handles string escaping, UTF-8
validation, and floating-point formatting. Detector metadata continues to come
from the same lazy tree used by summaries and pagination. Progress counters are
published in batches, with the exact count published on completion or failure.

The existing owned snapshot, cancellation, staging-directory cleanup, and atomic
no-overwrite publication remain in place. Object key order changes; the JSON
schema and parsed values do not. No retained findings are omitted.

## Measurements

Release, Visual Studio 2026 / MSVC 19.51, Windows 11 26300, Ryzen 7 5800H,
63.86 GiB RAM, nlohmann JSON 3.12.0. The baseline uses the exporter from
`3e4d19f3dba00dfcb1e517d73a09dfd3bada5ce9`, compiled with the same new benchmark
harness before changing the exporter. No build or test suite ran during these
final measurements. Small cases alternate executable order across three fresh
processes per version; the large case has one process per version. These are
warm filesystem-cache measurements, not physical disk durability timings.

| Output | Retained entries | Baseline job | Buffered job | Speedup | Baseline / buffered MB/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| 66,825,970 bytes | 810,027 | 7.227 s median | 0.717 s median | 10.1x | 9.25 / 93.20 |
| 668,205,976 bytes | 8,100,027 | 103.587 s | 12.756 s | 8.1x | 6.45 / 52.38 |

Jobs include snapshot creation, worker launch, serialization, writes, publication,
and waiting for the worker. They exclude fixture construction, the separate null
sink pass, and status-polling delay. MB/s uses decimal megabytes.

For the large buffered export, snapshot preparation took 75.4 ms, serialization
11,827.5 ms, stream writes/close 820.4 ms, and publication/cleanup 0.6 ms. The old
writer needed 102.269 s even when writing to the null device; the new writer took
12.494 s there. This isolates a substantial serialization/streaming cost rather
than attributing the old latency to output size or storage alone. Serialization
is wall time outside stream writes, not a CPU profiler measurement.

Peak private memory, sampled every 20 ms across both passes and snapshot creation,
was 15.13 / 15.11 MiB for the small baseline/buffered cases and 137.77 / 137.95 MiB
for the large case. Guards remained at 38 GiB process private memory, 6 GiB minimum
available memory, and a 600-second process deadline; none triggered. Snapshot
ownership was not reworked: its measured cost was small in this workload.

[Raw measurements](benchmarks/analysis-export-20261004.json) include per-run stage
times, memory, executable hashes, and output-validation results. Full output files,
build/test logs, and the sampling driver remain machine-local under
`D:/woby-builds/issue-108`.

## Scope and validation

The benchmark constructs fixed retained boundary-edge findings and duplicate
groups, with coordinates, escaped source names, nested face/point references,
and 20 members per duplicate group. Findings reference a small shared source
graph, so this isolates exporter throughput and does not reproduce large-model
topology memory or cache locality. The smaller workload uses 100,000 boundary
findings and 10,001 duplicate groups; the larger uses ten times as many boundary
findings and 100,001 groups.

Both versions report exactly the same byte and nested-entry counts, with
`allRetainedResults=true` and `detectionComplete=false` (manual intersections were
not run). Parsed JSON equality was checked on the small output. The large outputs
were compared as a stream of complete top-level collection records plus metadata,
ignoring object key order without loading either complete document into memory.

The Debug application and test targets build without warnings. All 893 CTest
cases pass, including the slow tests. Export coverage includes every detector on
both sides, nested collections beyond summary limits, unfinished/disabled/partial
detectors, maximum IDs, non-finite numbers, control characters and UTF-8, consistent
snapshots, cancellation after data is written, failed serialization cleanup/retry,
and concurrent no-overwrite publication. The benchmark driver also has a small
count/no-overwrite regression test. Optional GPU/headless integration tests were
not enabled in this build.

These are synthetic exporter measurements, not reruns of Powerplant or San Miguel.
They must not replace those models' original 94–116-second measurements. The new
`analysis.export-status` stage timings make a subsequent model-level comparison
possible without status-polling uncertainty; see [the command reference](ctl-commands.md).

## Reproduction

Build the opt-in benchmark using the supported local preset, and choose fresh
absolute output directories (the benchmark preserves its output for inspection):

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_benchmarks
New-Item -ItemType Directory D:/temp/export-small-new
New-Item -ItemType Directory D:/temp/export-large-new
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe export 100000 3 D:/temp/export-small-new
build/vs2026-vcpkg/bin/Release/woby_benchmarks.exe export 1000000 1 D:/temp/export-large-new
```

Each repetition writes to the OS null device first, then uses the production
background export job and a fresh destination. JSON lines on stdout report both
timings, retained entries, bytes, and the production job's stage measurements.
For a baseline comparison, apply only the benchmark harness/CMake changes to the
baseline revision, leaving `analysis_results.cpp` and its header at that revision.
