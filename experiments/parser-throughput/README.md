# Parser throughput investigation

This opt-in Release executable compares the actual legacy `ParseFile` and the
prototype's `ParseFilePrototype`. Timing ends when the parser returns; it includes
file opening/reading, numeric decoding, index reconciliation, allocation, merging,
and synchronous cleanup. Hashing output, fixture generation and input warming are
outside the timer. This is not a triangulation or full-viewer benchmark.

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_RAPIDOBJ_PROTOTYPE_BENCHMARK=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_parser_benchmark
python experiments/parser-throughput/run.py build/vs2026-vcpkg/bin/Release/woby_parser_benchmark.exe build/parser-results.json --rounds 5 --max-background-cpu 15
uv run experiments/parser-throughput/report.py build/parser-results.json doc/parser-throughput-performance
```

The runner currently expects the four local real models under `D:/temp/obj_tests`,
as does the workflow experiment. All synthetic inputs live in one unique temporary
directory, which is removed on normal completion or Python exceptions (forced
process termination can leave it behind). `--only quads lines cloud`
selects a subset. `--before saved.exe` adds the previous prototype executable.
`--sweep 256 1024 8192` additionally measures prototype block-size caps in KiB.
`--workers` gives both parsers the same worker budget (zero preserves their defaults).
Legacy has no public worker option, so this override exists only in the generated
benchmark header. Earlier archived campaigns predate this option; their metadata
and source hashes identify the old prototype-only worker setting.

Run on an idle machine after builds and tests complete. Every sample uses a fresh
process, warmed input and rotating variant order. The runner checks for known
build/test/viewer processes before and after each sample and retries detected
overlap. `--max-background-cpu 15` additionally waits for two consecutive 0.5-second
CPU samples at or below 15%, after warming the input. The default, 100, disables
this gate. A low threshold can wait indefinitely on a busy machine. Neither guard
detects every source of system load or short-lived interference during a sample.
Windows unbuffered file reads can still vary despite warming. Medians and ranges
are reported separately for every model, never hidden behind an aggregate score.

The comparison rejects missing/repeated rounds, invalid timings and mismatched
ordered geometry fingerprints. Position coordinates retain exact double bits.
UVs and normals are compared at legacy float precision, because the prototype
retains doubles. Shape names, face/line/point indices, face sizes, material IDs and
smoothing IDs are included. Material payloads and vertex colors are not covered
by this fingerprint; contract tests and full-viewer validation cover additional
behavior. The working regression flag is a per-model median above both 105% of
legacy and legacy + 0.1 ms. Five repeats are not a statistical equivalence proof.

`generate.py` instruments benchmark-only header copies. It fails if source anchors
drift. Production parsing has no new timers. There are no per-vertex clock calls.
`scan_ms` includes reading, scanning and collection; `merge_ms` includes allocation,
task construction and execution. `attribute_allocate_ms` measures attribute
setup before merge workers start. In the saved prototype this includes vector
value-initialization; legacy mostly reserves array storage. The fixed prototype
constructs positions in a merge task, overlapping index reconciliation, so that
cost now belongs to `merge_execute_ms`. Neither metric is a pure allocator timer.
Attribute setup and execution are subphases of merge.
Worker input/decode totals are sums across overlapping workers, not wall times.
Both persistent native ranges and legacy use reader submit/wait counters. The
older block path includes block allocation in input time; native-range setup and
boundary handling belong to worker decode time. Do not add these sums to wall time.
Residual time includes setup/cleanup. Legacy may defer large-buffer destruction to
a detached cleanup thread; the prototype completes its own cleanup synchronously.

For grammar unsupported by legacy RapidOBJ (weighted/freeform/continued input),
compare the complete Woby loaders with `experiments/load-workflow/run.py`; calling
the unsupported legacy parser alone would not be an equivalent successful load.

The [measured investigation](../../doc/parser-throughput-performance/README.md)
records fixes, the final corpus, ablations, ranges and remaining opportunities.
