# Large OBJ loading investigation

Measured on 27 September 2026 against woby commit
`0602f0415adf105290f93851be5451b22dedd0ed` (0.21.3). The added opt-in benchmark
targets instrument generated copies of the production sources. The application's
loader and its dependencies are unchanged.

The [measurement records](obj-loading-results.json) contain per-run timings,
memory counters, geometry checks, medians, ranges, and pinned revisions. Long
material-warning strings are abbreviated; timing and validation values are
unchanged. Complete local logs are in `build/obj-loading-*.jsonl`.

## Findings

**Keep RapidOBJ for now and optimize mesh conversion.** On these four models,
its native parser is about 3.0–3.6 times faster than fast_obj and 4.5–6.3 times
faster than the best measured optimized tinyobj variant. Woby spends roughly
77–83% of its measured CPU mesh-loading time in vertex mapping/construction and
compaction. SIMD text scanning or a parser swap does not address that work.

Values below are medians of three fresh-process Release measurements, except
the explicitly marked traditional tinyobj control, which has one run per model.
Seconds are wall time; file sizes use decimal GB and memory uses GiB. The model
contents were not modified or simplified.

| Model | File GB | Source positions | Source faces | Faces with >3 corners |
| --- | ---: | ---: | ---: | ---: |
| BearTrap | 7.109 | 37,890,645 | 75,771,986 | 0 |
| Bennu | 0.816 | 8,933,524 | 17,866,836 | 0 |
| Powerplant | 0.818 | 5,984,083 | 12,759,246 | 0 |
| San Miguel | 1.143 | 5,933,233 | 9,932,385 | 48,308 |

### Parser comparison

These rows include file I/O and parsing, with **triangulation disabled**.

| Parser / experiment | BearTrap s | Bennu s | Powerplant s | San Miguel s |
| --- | ---: | ---: | ---: | ---: |
| RapidOBJ, current native I/O | **6.513** | **0.835** | **0.631** | **0.838** |
| RapidOBJ, cached I/O experiment | 5.616 | 0.782 | 0.669 | 0.774 |
| fast_obj | 19.767 | 2.723 | 2.260 | 2.909 |
| tinyobj traditional `LoadObj` (single run) | 160.292 | 23.421 | 23.184 | 20.955 |
| tinyobj `LoadObjOpt`, all macros enabled | 33.885 | 5.279 | 4.350 | 4.977 |
| tinyobj `LoadObjOptTyped`, all macros enabled | 29.575 | 4.681 | 4.224 | 4.614 |
| tinyobj typed + float cache | 29.336 | 4.804 | 3.969 | 4.531 |

The float trie cache is not a consistent win: it slightly slows Bennu and helps
Powerplant. Arena output reduces memory and often time relative to `LoadObjOpt`,
but neither optimized tinyobj API approaches RapidOBJ on these inputs.
The traditional API is an auxiliary control; the repeated optimized API results
are the basis for comparing tinyobj's performance with RapidOBJ.
Every successful parser run agreed on the recorded counts, ordered index hash,
position bounds, and position sums. All twelve woby runs matched the source
position count and expected triangle count. These checks have the semantic
limits described below.

Cached RapidOBJ has lower medians on three models but is about 6% slower on
Powerplant. BearTrap native ranged from 4.611 to 7.282 seconds, while cached
ranged from 5.436 to 8.314 seconds. Those overlapping ranges and the deliberately
pre-read files make this an I/O experiment to investigate further, not evidence
for a universal change to the production reader.

### Peak memory

Median **peak working set** per fresh process:

| Parser / CPU pipeline | BearTrap GiB | Bennu GiB | Powerplant GiB | San Miguel GiB |
| --- | ---: | ---: | ---: | ---: |
| RapidOBJ | 8.271 | 1.620 | 1.169 | 1.128 |
| fast_obj | 5.892 | 1.044 | 1.019 | 0.767 |
| tinyobj traditional (single run) | 20.489 | 3.835 | 1.629 | 1.716 |
| tinyobj optimized | 27.675 | 5.292 | 4.009 | 3.968 |
| tinyobj typed | 23.862 | 4.293 | 3.302 | 3.387 |
| Woby `loadObjMesh`, including finalization | 9.967 | 2.181 | 1.904 | 1.680 |

Fast_obj is the memory-saving tradeoff among these parsers, with slower loading.
Tinyobj's extra footprint is not just mapped input: on BearTrap, median peak
**commit** was 26.223 GiB for optimized tinyobj, 23.161 GiB for its typed API,
9.827 GiB for RapidOBJ, and 7.264 GiB for fast_obj. Allocation strategies were
audited in source; allocation call counts were not instrumented.

### Woby's current CPU pipeline

| Stage | BearTrap s | Bennu s | Powerplant s | San Miguel s |
| --- | ---: | ---: | ---: | ---: |
| Parse inside this load | 6.222 | 0.745 | 0.692 | 0.803 |
| Triangulate / inspect faces | 0.253 | 0.065 | 0.043 | 0.038 |
| Validate/copy source points | 0.408 | 0.112 | 0.068 | 0.071 |
| Build/index render vertices | **22.698** | **4.803** | **3.474** | **2.782** |
| Validate/generate normals | 1.942 | 0.536 | 0.056 | 0.049 |
| Bounds | 0.479 | 0.108 | 0.134 | 0.107 |
| Meshoptimizer compaction | **9.130** | **2.090** | **2.375** | **1.872** |
| Total `loadObjMesh` | **41.247** | **8.449** | **7.082** | **5.806** |
| Total min–max | 38.288–45.150 | 7.739–9.298 | 6.361–8.508 | 5.645–5.996 |

Per-stage medians need not sum to the median total; total also includes local
destruction and small untimed boundary work. Parse medians differ from the
parser-only table because they come from separate runs. This is CPU mesh import,
not time until the viewer displays the model. San Miguel produces 9,980,699
triangles after woby triangulation.

Using the median stage costs as an approximation, even eliminating parsing
entirely would improve this CPU pipeline by only about **1.10–1.18x**. Halving
vertex construction and compaction would be a much larger opportunity, but that
speedup has not been implemented or measured here. Timing ranges are substantial;
three repetitions do not provide a statistical confidence interval or control
CPU thermals and unrelated operating-system activity.

## What woby currently does

The CPU path is `loadModel` → `loadObjMesh` → RapidOBJ parsing → triangulation →
source geometry capture → render-vertex indexing → `finalizeMesh`.
`createUiFileState`, analysis preparation, GPU upload, and rendering happen later.
The existing `perf model_cpu_load` log calls the entire `loadModel` duration
`parse_ms`; that field is **not** a measurement of the text parser alone.

Implementation references: [OBJ conversion](../src/obj_mesh.cpp),
[mesh finalization](../src/model_mesh.cpp),
[batch scheduling](../src/background_load.cpp), and
[vendored RapidOBJ](../third_party/rapidobj/include/rapidobj/rapidobj.hpp).

### Multithreading

The vendored RapidOBJ is version 1.1.0 at
`fe4c779314b0daed19530c8b5aa323c789d08f81`, with woby's Unicode-path and polygon
winding fixes. For files above 1 MiB it divides 256 KiB blocks between
`std::thread::hardware_concurrency()` parsing workers: 16 on this machine. Each
worker overlaps reads and parsing using two aligned buffers. Merge tasks also run
in parallel. Polygon triangulation can dispatch parallel work; all-triangle
inputs still undergo a face-count scan. Material parsing may run asynchronously.
Large temporary parse buffers are freed on a detached recycling thread.

The source-position copy, tuple-to-vertex hash table, mesh construction, normal
generation, bounds passes, and meshoptimizer compaction in woby are serial.
Loading on a background thread keeps the UI responsive but does not parallelize
these steps. Separate files larger than 1 MiB are processed sequentially. The
up-to-four-file prefetch applies only to small OBJ/STL files in batches of at
least eight, so it does not help this dataset.

### SIMD

The vendored RapidOBJ does not contain explicit SSE/AVX intrinsics or a switch to
enable them. Its embedded fast-float conversion includes word-at-a-time integer
digit processing; this is distinct from an AVX2 newline scanner. Compiler
autovectorization and optimized runtime `memcpy` can still use vector instructions.
Woby's loader has no hand-written SIMD kernel or project-wide AVX2 requirement.
Source inspection establishes those facts; it does not measure an instruction
mix or prove that machine code is entirely scalar.

### Allocations and data copies

RapidOBJ uses per-worker contiguous growable buffers, initially at least 4,096
elements, then approximately doubles capacity and copies existing elements. It
allocates final contiguous arrays and merges the worker buffers into them.
This avoids per-face heap nodes but is not a zero-copy parser.

Woby reserves final index arrays and estimates render-vertex capacity from the
number of source positions. Its custom open-addressed table stores 12-byte
position/normal/UV keys contiguously, with 32-bit bucket references and a maximum
75% load factor. Buckets have power-of-two capacity. UV or normal seams can cause
more render vertices than source positions and trigger growth.

Source positions are copied from floats into 24-byte double-precision points for
provenance and analysis. Each triangle corner gets both a source index and a
render index. Render vertices are 32 bytes each. Missing normals are generated;
bounds require two vertex passes. `compactMesh` then allocates a remap and another
vertex array and performs value-based deduplication using meshoptimizer. This has
different semantics from deduplicating OBJ index tuples: distinct source indices
can contain identical attribute values. It cannot simply be removed without
considering that behavior.

The parsed OBJ and tuple table currently remain alive through finalization.
There is no woby OBJ arena, PMR resource, custom general-purpose heap, or
memory-mapped output store. Reserving arrays and replacing node allocations with
the dense hash table are the principal woby-side allocation optimizations.

### File I/O and virtual memory

Large Windows inputs use `CreateFileW` with `FILE_FLAG_NO_BUFFERING` and
`FILE_FLAG_OVERLAPPED`, plus 4 KiB-aligned buffers. Reads bypass the Windows file
cache and overlap work. Alignment serves direct I/O requirements; it is not
evidence of SIMD parsing. This does **not** use `CreateFileMapping`/`MapViewOfFile`.
At or below 1 MiB, woby switches to a buffered `ifstream` and RapidOBJ's serial
`ParseStream` API.

Memory-mapped files use a virtual address range backed by file pages, which the
OS brings into memory as accessed. This can avoid a whole-file staging copy but
does not eliminate parsing, page faults, or allocations for decoded geometry.
That is separate from a virtual filesystem API: fast_obj provides file callbacks
that could read archives or other sources; woby currently passes ordinary paths.

## Comparison configuration

All competitors are pinned and built in MSVC Release mode (`/O2`, `NDEBUG`,
release CRT) using the `vs2026-vcpkg` preset. No fast-math changes or alternate
heap allocator are used.

| Library/API | Threads | SIMD | Input | Allocation strategy |
| --- | --- | --- | --- | --- |
| Woby's RapidOBJ | Automatic, 16 here | Fast-float digit processing; no explicit vector scanner | Parallel overlapped unbuffered reads | Growing per-thread buffers, final merge arrays |
| fast_obj 1.3 | One | No explicit SIMD option | Buffered `fread`, 64 KiB chunks | Geometrically growing arrays through `realloc` |
| tinyobjloader `LoadObj` | One | Optimized float conversion; optimized API scanner switches do not parallelize this API | Memory mapping enabled | Standard vectors and intermediate face storage |
| tinyobjloader `LoadObjOpt` | Automatic, 16 here | AVX2 newline scanning and fast-float | Memory mapping enabled | Line tables, per-thread vectors, merged arrays and per-shape copies |
| tinyobjloader `LoadObjOptTyped` | Automatic, 16 here | Same AVX2 and fast-float | Memory mapping enabled | Arena-backed final arrays, shape ranges; optional arrays allocated when needed |

Fast_obj is pinned at
[`d620667f10a548dee94dbc8c144bb22f79162176`](https://github.com/thisistherk/fast_obj/blob/d620667f10a548dee94dbc8c144bb22f79162176/fast_obj.h).
Its configurable `FAST_OBJ_REALLOC`/`FAST_OBJ_FREE` hooks retain their standard
allocator defaults. It has no multithreading, SIMD, or mmap optimization macro to
turn on. `FAST_OBJ_UINT_TYPE` changes the representation, not an optional speed
path, and retains its 32-bit default.

Tinyobjloader is pinned at
[`45636bdcef1a4fec140346b90c0b50bf0bc3e23b`](https://github.com/tinyobjloader/tinyobjloader/blob/45636bdcef1a4fec140346b90c0b50bf0bc3e23b/tiny_obj_loader.h).
The benchmark defines `TINYOBJLOADER_USE_MULTITHREADING`,
`TINYOBJLOADER_USE_SIMD`, `TINYOBJLOADER_USE_MMAP`, and
`TINYOBJLOADER_ENABLE_EXCEPTION`. The tinyobj translation unit uses `/arch:AVX2`;
the JSON output verifies that its AVX2 branch was compiled. Embedded fast_float
8.0.2 remains enabled. The optional runtime float-token trie cache is tested
separately (`float_cache=true`, 1,024 nodes per thread, `fp32_cache=true`).
Double precision and Earcut are not speed switches: attributes use float32, and
triangulation is disabled for parser comparisons. The optimized APIs use fan
triangulation when requested, which is not a drop-in replacement for woby's
winding-preserving Earcut handling of general polygons.

The pinned tinyobj fast-float shim fails MSVC's C++20 constexpr check because its
`distance` helper is not constexpr. Competitor translation units use C++17 to
retain the unmodified upstream implementation and all runtime optimizations.
Woby/RapidOBJ remain C++20. The standard tinyobj API also defaults to a 256 MiB
input limit; `TINYOBJLOADER_STREAM_READER_MAX_BYTES` is set to 16 GiB for this
64-bit benchmark. The initial default-limit failures are retained in the raw
data and excluded from timing summaries.

The newer independent pure-C tinyobj implementation and the old experimental
loader are outside this comparison; the current C++ standard, optimized, and
arena-backed APIs are included.

## Measurement method

Machine: AMD Ryzen 7 5800H, 8 cores/16 logical threads, approximately 64 GiB RAM,
Windows, Visual Studio 2026 v145, SKHynix HFS001TDE9X084N NVMe storage. Models are
the four OBJ files in `D:\temp\obj_tests`. Adjacent referenced MTL files are absent;
all loaders are allowed to import geometry despite that absence. These results
do not measure material or texture loading.

Each measurement is a fresh process, run serially without a concurrent build,
test suite, viewer, or another benchmark. The runner pre-reads a model once per
round and rotates backend order. These are cache-conditioned repeated loads,
not controlled cold-storage measurements. RapidOBJ's native Windows reader
bypasses the file cache. A generated experimental header removing only
`FILE_FLAG_NO_BUFFERING` provides the `rapid-cached` comparison; it retains
overlapped I/O and all parser behavior.

The traditional tinyobj control was run separately after raising its input cap,
once per model. Its subsequently started second-round run was stopped and is
excluded. Main comparisons use three completed repetitions per variant; the
traditional control should not be compared as an equally precise median.

Parser timing covers file opening, I/O/page faults, parsing, and the API's own
merging. It excludes triangulation, benchmark validation, JSON output, and
destruction of the returned data. RapidOBJ can also defer its temporary-buffer
cleanup past the API return. The full `woby` timing instead calls the current
`loadObjMesh`, including triangulation, normal generation, bounds, compaction,
and local parser-data destruction. It still excludes scene-state construction,
detectors, GPU work, and presentation.

Post-timing validation records position/normal/UV counts, polygon and corner
counts, expected triangulated face count, position bounds and sums, and an
ordered 64-bit hash of normalized position/normal/UV index tuples. These checks
detect truncation and index disagreements; they are not exhaustive equivalence
tests of materials, names, smoothing groups, colors, or floating-point values.
Windows process peak working set and peak commit are recorded separately. Working
set can include mapped input pages; commit and working set measure different
things. Peaks also include validation and cleanup, with no prior load in the
same process.

## Optimization priorities

1. **Optimize woby's tuple lookup and vertex construction first.** A direct
   source-position-to-render-index array is appropriate when faces have no
   normal or UV indices; Bennu is the large real fixture for this case. Assign
   IDs on first use to retain ordering. For inputs with seams, investigate a
   direct primary entry per source position plus a secondary table for alternate
   normal/UV combinations. These are proposals, not measured improvements.
2. **Reduce finalization's extra deduplication work where equivalence is proven.**
   Measure how many vertices the value-based compaction actually removes. A
   bypass requires proof that full attribute values are already unique. Missing
   normal generation and shared source provenance constrain when vertices may
   be merged. Preserve triangle order and annotation identity.
3. **Shorten temporary lifetimes.** Release the parsed OBJ and tuple table once
   mesh conversion is complete, before allocating compaction's remap and output
   vertex arrays. This is primarily a peak-memory improvement to measure; an
   arena alone would not remove duplicate representations or hashing work.
4. **Evaluate cached parallel reads separately for cold and repeated opens.**
   The benchmark's cached variant changes one Windows I/O flag, retaining the
   parser. Warm-file results cannot establish cold-storage performance. Do not
   replace parallel `ParseFile` with serial `ParseStream` for large models based
   only on a cache observation.
5. **Parallelize conversion only with an explicit deterministic merge.**
   Independent position copies and bounds reductions are straightforward but
   small contributors. Naively sharing the current hash table would add races;
   per-shape tables could duplicate vertices shared by groups and change normal
   behavior. Any parallel conversion must retain first-use IDs, seams, source
   topology, winding, and node ranges.

The SIMD/mmap/arena comparison does not isolate the causal benefit of each
technique: these libraries differ in algorithms and output storage as well.
Tinyobj's typed arena output still has line tables, per-thread parsing buffers,
and merge work. Explicit AVX2 scanning and memory mapping therefore do not by
themselves imply lower time or memory than RapidOBJ's block-based reader.

Regression coverage for a production optimization should include shared
positions with different UVs/normals, duplicate attribute values with different
OBJ indices, missing normals, negative indices, empty/unreferenced positions,
multiple groups, concave polygons and winding, and Unicode paths. Add large
model timing and peak-memory checks alongside those semantic tests.

## Reproduce

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_OBJ_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_obj_benchmark_native woby_obj_benchmark_cached
uv run tests/obj_loading/contract_test.py build/vs2026-vcpkg/bin/Release/woby_obj_benchmark_native.exe
uv run tests/obj_loading/run.py --bin build/vs2026-vcpkg/bin/Release --models D:/temp/obj_tests --output build/obj-loading-results.jsonl --rounds 3
```

The optional CMake targets fetch the exact competitor commits; a normal build
does not fetch or link them. Results append to the output file. Use a new output
filename for a distinct experiment. `--backends` selects variants;
`--threads` controls optimized tinyobj APIs only. `woby-cached` also works for a
full-pipeline buffered-I/O experiment. This benchmark's tinyobj executable
requires an AVX2-capable x64 CPU on this Windows configuration.

To reproduce the original sample counts, pass `--backends rapid rapid-cached
fast tiny-opt tiny-typed tiny-typed-cache woby` for the three-round comparison,
then use a separate output file with `--backends tiny --rounds 1` for the
traditional control. The default command above instead repeats all listed APIs
three times, with the input-limit fix already enabled.

## Validation of this change

- `cmake --build --preset vs2026-vcpkg`: Debug build succeeded without warnings.
- Release benchmark targets built without warnings after the documented C++17
  compatibility configuration.
- `ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure`: **625/625
  passed**, including slow and graphics tests, in 243.48 seconds.
- The new benchmark contract checks untriangulated polygons, negative indices,
  normal/UV index preservation, all parser modes, woby stage instrumentation,
  invalid arguments, sweep output, and failure reporting. Its fixtures and
  output share one unique temporary directory and are cleaned on failure too.
- The contract also passed with the Release executable. No tests were added for
  document content.

Local verification logs: `build/obj-bench-debug.log`,
`build/obj-bench-release-final.log`, and `build/obj-bench-ctest.log`.
