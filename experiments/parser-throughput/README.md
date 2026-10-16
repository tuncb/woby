# Parser throughput investigation

This opt-in Release executable compares the actual legacy `ParseFile` and the
prototype's `ParseFilePrototype`. Timing ends when the parser returns; it includes
file opening/reading, numeric decoding, index reconciliation, allocation, merging,
and synchronous cleanup. Hashing output, fixture generation and input warming are
outside the timer. This is not a triangulation or full-viewer benchmark.

The executable also accepts `prototype-memory`. It allocates and reads the full
file before the parse timer. It reports that preparation time and retained input
size separately. Include both times and the extra memory when comparing a load.
Windows records include parse CPU time, private memory, peak working set, and
logical read bytes. The output hash remains outside the parse timer.

`WOBY_PARSER_FASTFLOAT_HEADER` can select a supplied standalone fast_float header
for the opt-in `woby_parser_benchmark_modern` target. This replaces the embedded
decoder in generated research headers. It does not change production parsing.

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
UVs and normals use legacy float precision in `fingerprint`, because the prototype
retains doubles. A separate `precise_fingerprint` checks every double bit in
positions, UVs and normals between prototype builds. Shape names, face/line/point
indices, face sizes, material IDs and smoothing IDs are included in both hashes.
Material payloads and vertex colors are not covered
by this geometry fingerprint; contract tests and full-viewer validation cover additional
behavior. The working regression flag is a per-model median above both 105% of
legacy and legacy + 0.1 ms. Five repeats are not a statistical equivalence proof.

Use `woby_parser_benchmark prototype-colors path.obj` to include packed vertex RGB
decoding and storage. `prototype` retains the validation-only measurement. Color
runs report `vertex_color_count`, `vertex_color_bytes`, and a separate
`color_fingerprint`. Missing color is zero; authored RGB has a high byte of 255.
Compare the color fingerprint as well as both geometry fingerprints when testing
two color decoders.

Build the opt-in `woby_parser_color_reference` target to compare the integer
color decoder against the generic numeric color path. Both targets use the same
output storage and compile flags. The reference change exists only in a generated
benchmark header; production always uses the optimized decoder.

`color_compare.py --fast path/to/woby_parser_benchmark --generic
path/to/woby_parser_color_reference --before path/to/saved_parser --output
colors.json model.obj` runs five rounds in rotating order. It warms each input,
waits for an idle machine, checks both geometry hashes and the color hash, and
records CPU time and memory as well as wall time. It also creates a temporary
two-million-vertex decimal-color input and removes it after the run. The optional
`--before` binary must use the earlier geometry-only benchmark interface.

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
Legacy uses reader submit/wait counters. The prototype uses persistent workers
with a bounded queue of blocks. Its input time includes block allocation and
native reads. Decode time includes buffer sizing, numeric conversion and boundary
selection. Earlier prototype measurements used long native ranges and reader
submit/wait counters. Do not compare these input/decode subphases as equal scopes
or add overlapping worker sums to wall time.
On Windows, the benchmark also records parser process CPU time, peak working set
and peak committed memory before output hashing. Peak commit is not resident RAM.
Residual time includes setup/cleanup. Legacy may defer large-buffer destruction to
a detached cleanup thread; the prototype completes its own cleanup synchronously.

For grammar unsupported by legacy RapidOBJ (weighted/freeform/continued input),
compare the complete Woby loaders with `experiments/load-workflow/run.py`; calling
the unsupported legacy parser alone would not be an equivalent successful load.

The [measured investigation](../../doc/parser-throughput-performance/README.md)
records fixes, the final corpus, ablations, ranges and remaining opportunities.

The [15 October 2026 parser report](../../doc/benchmarks/obj-parser-20261015/report.html)
compares the production parser before and after the numeric, buffer, worker and
index-copy changes. It includes raw samples, full-precision output checks, CPU
import results and build/test evidence.

The [16 October 2026 vertex-color report](../../doc/benchmarks/obj-vertex-colors-20261016/report.html)
compares the cost of retaining RGB with the previous validation-only path. It also
compares the integer color decoder with numeric conversion and records render,
fallback, scene-reload and GPU checks.
