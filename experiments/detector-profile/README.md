# Detector performance probes

These programs call the production CPU analysis functions. They do not change the
viewer. Build them only when `WOBY_BUILD_BENCHMARKS=ON`, on Windows with MSVC.

| Program | Purpose |
| --- | --- |
| `woby_detector_probe` | Baseline timings with the production library. |
| `woby_detector_profile` | The same operations with coarse timing scopes in generated copies of source files. |
| `woby_detector_queries` | Counts distance-tree work and intersection predicate paths. Can select distance query batches. |
| `woby_detector_tokenref` | Tests one change: pass the quality cancellation token to its local check helper by reference. |
| `woby_detector_max3` | Tests nested two-value maximum operations for the three squared edge lengths in the distance degeneracy check. |

The last three programs are experiments. Do not use their times as production
baseline times. The query program with `--query-stride` greater than one does not
calculate valid distance statistics. It keeps the full target trees and output
allocations, and selects every Nth batch of 128 source triangles. Each selected
batch retains the production query order, four samples per triangle, and nearest
face hints. Histogram buckets count work per query, not time per query.

`generate.py` checks every source anchor. Configuration fails if an anchor moves
or matches more than once. The generated manifest records source and output
hashes. Generated files stay in the build directory.

## Build and check

Use the local Visual Studio preset. For example:

```powershell
cmake --preset vs2026-vcpkg -B D:/woby-builds/detector-analysis `
  -DWOBY_BUILD_BENCHMARKS=ON -DWOBY_TEST_HEADLESS=ON `
  '-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi /Zo' `
  '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG:FULL /INCREMENTAL:NO'
cmake --build D:/woby-builds/detector-analysis --config Release `
  --target woby_detector_probe woby_detector_profile woby_detector_queries woby_detector_tokenref woby_detector_max3 woby woby_tests -j6
ctest --test-dir D:/woby-builds/detector-analysis -C Release `
  -R '^woby_detector_profile_contract$' --output-on-failure
D:/woby-builds/detector-analysis/bin/Release/woby_tests.exe --no-colors=true
```

The contract tests check native/scoped results, analytical grid distances,
expanded/indexed input, worker budgets, cache ownership, positive closed-solid
differences, query selection, token controls, rejected geometry, and changed
source anchors. Fixtures use absolute paths within a unique temporary directory.

## Collect

```powershell
uv run experiments/detector-profile/run.py `
  D:/woby-builds/detector-analysis/bin/Release/woby_detector_probe.exe `
  D:/woby-builds/detector-analysis/measurements --name bus-native -- `
  --model D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj --runs 3 `
  --operations source,auto,quality,intersections,distance,difference
```

Each name must be new. The runner saves commands, executable hashes, matching
symbols, dependencies, source-generation manifests, process memory samples,
external CPU activity, operation results, and errors. It stops its own process
tree at the time or memory limit. It does not change the system power plan.
`matrix.py` runs each named group in sequence. `--list` shows the commands first.
An existing directory is retained and skipped, including failed captures; inspect
its metadata before deciding whether to make a new capture.

Use `--profile branch`, `data`, `l2`, `tlb`, `frontend`, `backend`, `prefetch`, or `math` to collect at most six
AMD core events at a time. Use `hardware.py CAPTURE_DIRECTORY` after collection to
align samples to timing scopes. Use `--profile timechart` for frequency/power or
`--profile vtune` for an independent software sample capture. Tool availability
depends on the installed drivers and processor. Preserve failures as evidence.

`application.py` measures the real headless viewer through CLI acknowledgments.
Its two-sided comparison uses identical A/B input. It includes application
preparation and command costs. It is a separate measurement from the CPU probes.

## Interpret

- `auto` includes the nine default detectors on side A. Intersections and quality
  are separate operations. Distance uses both sides.
- Side B is a copy shifted in Z by `--shift` times the bounding-box diagonal. The
  default ratio is 0.001. This is a controlled stress case, not a repaired model.
  Record the absolute shift in `mesh.shift`. A zero shift avoids mesh rebasing.
- `--bump R` adds a local Z displacement to B, with peak height `R*diagonal`.
  Its XY support has radius `0.2*diagonal` around the first indexed vertex. The
  weight is `(1-r*r)^2` inside that support and zero outside. Connectivity stays
  fixed. This is a controlled repair proxy, not an authentic repaired input.
  Its owned geometry increases memory use compared with a shared shifted copy.
- `--grid N` creates an open square with `2*N*N` triangles. `--sphere N` uses
  Manifold's sphere generator. Use the recorded triangle count for comparisons.
- `--faces N` keeps a prefix and remaps its vertices. This changes topology and
  spatial coverage. Do not describe it as a complete large-model result.
- `--expanded` repeats render vertices but preserves source provenance.
  `--shuffle` changes render-face order within each part; part membership and
  detector source order are unchanged.
- `--exact` changes vertex identity rules. Different detector counts can be
  correct. It is not a performance-equivalent mode switch.
- `--source-batches N` divides the imported parts into up to N contiguous
  detector source batches, with shared source arrays and unchanged geometry.
  One batch is the default. One part per batch tests per-part costs. This
  control does not reproduce hierarchy-root merging for arbitrary imports.
  It changes detector source records, not application world-mesh assembly or
  render-vertex batch tags. It is not a full per-volume quality-mode simulation.
  Duplicate and topology domains change, so compare counts as well as time.
- A live stop source is the default, as in the application. `--empty-token` is a
  control. It does not represent application cancellation behavior.
- `--cancel-after-ms N` requests cancellation during each operation. The result
  records the actual request time and return delay, including stack cleanup.
  Zero requests cancellation before computation. This control excludes cache
  preparation and cannot use an empty token.
- `--helpers N` reserves the other shared worker slots. The calling thread can
  also work. CPU time divided by wall time is average active logical CPUs, not
  a utilization percentage of the whole machine.
- `--high-qos` explicitly disables execution-speed throttling for this probe
  process before loading. It records the Windows process policy before and
  after the request. It does not change the system power plan or other processes.
- `topology-warm` and `intersections-warm` recompute findings with retained input
  geometry. They are not unchanged UI cache hits. Intersection trees are rebuilt.
- `source` measures shared ownership inside `comparisonSourceMesh`. It excludes
  world-space snapshot assembly in the application.
- The default intersection limits are 10,000 pairs and 1,000,000 candidate tests.
  A truncated result is incomplete. Difference creation has its own validation
  limits and can reject an input before any Boolean operation.
- Each result is released after the operation timer. Release time is recorded
  separately. OS lifetime memory peaks include loading and earlier operations.
  Scoped sampled memory is available when the observer clock was recorded.
  The observer's 250 ms sleep is not its sample period. Collection adds time;
  use recorded timestamps and the exported interval summaries.
- The counter program reports broad-phase box tests, fast separation decisions,
  adaptive predicate decisions, and rational fallbacks for each synchronous
  intersection inspection. These counts retain the production decisions.
- Nested scopes overlap. Do not add a parent to its children, or add concurrent
  workers as if they ran in sequence. Short CPU times have Windows timer noise.
- Hardware counts are sample count times sampling period. They are estimates.
  Timeline bucket indices use the translated `aggregateInterval`; long captures
  can use 2 ms buckets rather than 1 ms. The analyzer reads this value explicitly.
  Alignment sensitivity is retained at plus/minus 16 ms. Sampling skid, inlining,
  identical-code folding, and unresolved symbols limit instruction attribution.
  Profiled timings include profiler overhead. Use native captures for latency.
- Cache-fill sources do not give a general cache hit rate or total DRAM bandwidth.
  Pipeline event rates can overlap. Unsupported IBS, L3, or memory-controller
  sources must remain unavailable; do not infer precise latency from core PMCs.

Use `summarize.py ROOT --output DESTINATION` to make JSON and CSV tables. The
summary retains each run and distinguishes errors, truncation, counters, and
experimental code from baseline captures.

After measured workloads have stopped, export all profiler reports, portable
evidence, and figures with:

```powershell
uv run --no-project python experiments/detector-profile/postprocess.py `
  D:/woby-builds/detector-analysis/measurements doc/benchmarks/detectors-20261017
```

`--steps hardware,power,vtune,code,verify,evidence,plots` selects processing stages. Hardware
analysis is reused only when the analyzer source hash matches. The portable ZIP
contains raw JSON, logs, available generated source, compiler options, and symbol
manifests. It excludes the large binary databases, model files, executables, and
PDB files. Keep the local capture directory for a full profiler session.

The result tables retain event periods and sample counts. `hardware-summary`
holds rates, `hardware` holds events, `phases` holds scope times, `queries` holds
tree work, `predicates` holds intersection paths, `power` holds system-wide
frequency and power samples summarized by phase, `application` holds CLI timing,
and `cancellation` holds actual stop-request delays. Query histogram
quantiles are intervals from logarithmic buckets, not exact percentile values.
