# RapidOBJ

`include/rapidobj/rapidobj.hpp` is vendored from
[guybrush77/rapidobj](https://github.com/guybrush77/rapidobj) at commit
`fe4c779314b0daed19530c8b5aa323c789d08f81`.

The header is distributed under the MIT license in `LICENSE` and retains its
embedded third-party notices.

Woby changes are applied directly to the upstream header:

- Open Windows paths with `CreateFileW` so Unicode filenames work.
- Drain outstanding block reads before freeing their destination buffers on all
  parser exits, including errors and exception unwinding. Use the file handle for
  Windows read completion and detect null event-creation failures.
- Check aligned-buffer allocation and propagate parse, merge, and triangulation
  worker exceptions through joined futures. Allocation or thread creation failure
  must not leave detached workers using the caller's buffers.
- Restore each source polygon's winding after Earcut triangulation, using signed
  areas in the same projection for the polygon and its triangles.

- Parse, merge, and triangulate positions as doubles. Normals, UVs, colors, and
  material values remain floats. Woby recenters positions before triangulation
  and creates float vertices only afterward.
- Offset the negative-reference flag pointer when splitting an index merge task;
  each subtask must consume flags from the same range as its indices.
- Allow merge to fill caller-owned typed coordinate buffers and triangulate a
  view of those positions. Existing `ParseFile`/`ParseStream` ownership is unchanged.
- Allow the native file handle to use buffered positioned reads, so prototype
  workers can read arbitrary block boundaries through the existing reader and
  pending-I/O lifetime guard. These handles allow concurrent read-only opens for
  prefetched instances of the same OBJ. The legacy file reader keeps its existing mode.

`tests/obj_mesh_tests.cpp`, `tests/freeform_tests.cpp`,
`tests/coordinate_origin_tests.cpp`, and the Windows-only
`tests/rapidobj_reader_tests.cpp` cover these changes. The standalone reader tests
delay I/O completion to check buffer lifetime deterministically. Preserve these
changes when updating the header. CMake includes this directory as a system include path and links Threads
for the parser's Linux threading support.

## Issue 112 prototype

`include/rapidobj/prototype.hpp` adds an opt-in scanner around RapidOBJ's existing
polygon decoder, index reconciliation, merge scheduling and triangulation.
It reads file, stream and borrowed memory input through the same logical-line
scanner. Workers scan raw blocks and decode complete statements directly from
views of those blocks. File workers perform positioned native reads; memory
workers borrow input spans; stream input fills bounded blocks on the caller.
Only block-boundary fragments and continued statements need reconstruction.
Ordered collection reconciles these fragments, physical line numbers and
declaration counts before merging numeric freeform records. Woby resolves their
state using its existing spline and trimming algorithms. There is no parse-error
retry, filtered polygon text, or second numeric parse.

The caller supplies position, UV and normal vectors. Merge writes those buffers
directly; Woby localizes the position pool in place, triangulates through a view,
and moves that allocation into source geometry. The prototype retains UVs and
normals as doubles until packing, preserving freeform control precision. This
has a temporary-memory cost relative to the legacy polygon parser's floats.
Sparse explicit position weights are retained only until analytic patches exist.
Woby's separate capacity preflight for potentially oversized files remains active.

The application switch is OFF by default. To build and exercise the prototype:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_RAPIDOBJ_PROTOTYPE=ON -DWOBY_TEST_HEADLESS=ON -DWOBY_BUILD_RAPIDOBJ_PROTOTYPE_BENCHMARK=ON
cmake --build --preset vs2026-vcpkg --config Debug
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure
```

`tests/obj_prototype_tests.cpp` belongs to the regular unit suite and compares
both readers directly. The optional `woby_obj_prototype_contract` target builds
those cases separately for faster development. `loadObjMeshLegacy` and
`loadObjMeshPrototype` remain explicitly callable with either switch setting.
In-memory prototype loading uses a material-name-only policy: stable per-face
IDs without filesystem access. File loading resolves optional MTLs beside the OBJ.

For an idle-machine CPU comparison, build the benchmark in Release and run:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_obj_prototype_benchmark
python tests/obj_prototype_benchmark.py build/vs2026-vcpkg/bin/Release/woby_obj_prototype_benchmark.exe D:/models/example.obj --rounds 3 --output build/prototype-results.json
```

Use `--reference-executable absolute/path/to/previous-benchmark.exe` to include
the prototype from a saved executable as `prototype_before`. With three rounds,
each reader occupies each position in the comparison order once.

The runner adds 200 small files and mixed weighted polygon/freeform fixtures,
rotates reader order, warms input before every fresh process, and requires matching
ordered geometry fingerprints. Windows reports peak working set and peak process
commitment. `parsed_chunk_bytes` counts live elements, not allocation capacities;
`chunks` counts raw input blocks. `peak_inflight_text_bytes` bounds dispatched
input spans and aligned read buffers, not total process memory; borrowed memory
spans do not represent additional allocations. The workload measures CPU loading, excluding GPU upload, first
visible frame and annotation preparation.

This prototype bounds in-flight source text, not all temporary geometry. Parsed
worker buffers still live until merge. File reads and scanning run in parallel;
only boundary reconciliation, ordered collection and material dispatch run on
the caller. Cancellation is checked while reading, dispatching and waiting for
workers. Workers poll a stop flag and pending native reads are joined before
their buffers can be freed. Merge and triangulation still
need finer cancellation granularity. Render-position duplication, annotation
snapshots, freeform append copies and shared immutable asset publication remain
separate work for issue 112. The old source-anchor-based mapping experiments have
not been migrated to the new loader layout; use this comparison executable.

### Initial measurements (2026-10-04)

Measured against the explicit legacy loader in the same Release executable,
based on Woby `7bed51d`, on a Ryzen 7 5800H (8 cores / 16 threads) with 64 GiB RAM.
Other build, test and viewer processes had finished before measurement. These
are medians of three fresh-process runs per reader, with warm input, alternating
reader order, and the default worker limit. All 36 ordered geometry fingerprints
matched within their workload. The Debug application build was warning-free and
all 947 CTest checks passed with the prototype enabled.

| Workload | Legacy CPU load (ms) | Prototype CPU load (ms) | Legacy peak commit (MiB) | Prototype peak commit (MiB) |
| --- | ---: | ---: | ---: | ---: |
| 200 small files | 133.5 | 128.4 | 1.7 | 1.4 |
| Mixed weighted polygons + rational curve | 653.0 | 104.9 | 29.6 | 24.9 |
| BusGameMap, 75.6 MB | 240.1 | 1006.3 | 173.1 | 165.4 |
| Semantic3D, 10 million points | 1457.8 | 5390.0 | 1187.1 | 958.5 |
| Bennu, 815.9 MB triangles | 3646.7 | 13673.7 | 2415.1 | 2076.4 |
| Powerplant, 817.9 MB with seams | 4499.0 | 12756.8 | 2333.1 | 2206.4 |

The mixed fixture benefits from removing fallback rereading and text
reconstruction. Direct position ownership saves about 229 MiB of peak commitment
on the point cloud. Retained main mesh-buffer capacities are identical between
readers: this prototype removes a temporary position copy, not the retained
render-position and analysis representations.

Ordinary large inputs took 2.8 to 4.2 times as long in the initial implementation
at `a7286ac`, which scanned and reconstructed every line on the caller before
dispatching decoding work. These measurements predate parallel block scanning.
Restoring their throughput is required before enabling the prototype by default. The result
supports modifying RapidOBJ for the grammar and ownership contract, but does not
justify replacing the current production loader yet. Small-file differences are
too small to treat as a meaningful speedup.

The local `build/prototype-results.json` artifact contains all raw timings,
prototype parse/freeform/preparation breakdowns, peak working set, peak process
commitment, retained capacities and fingerprints. It is generated output and is
not committed. GPU upload, first visible frame, annotation preparation and a
measured cancellation-latency bound remain outside this comparison.

### Throughput fix (2026-10-05)

The serial scanner was starving the decoding workers: it read and reconstructed
every physical line before dispatch, and numeric decoding scanned token endings
twice. The replacement dispatches raw blocks, reads and scans file blocks on the
workers, and decodes ordinary statements directly from their input views. Only
boundary fragments and continued statements are reconstructed. Numeric decoding
finds and validates the delimiter in one pass. Buffered native Windows handles
share read access so concurrent prefetches can load the same file.

The comparison below uses the same hardware and default worker limit described
above, with three fresh Release processes per reader and workload. `Before` is
the saved prototype executable from `a7286ac`. Reader order rotates, and input is
warmed immediately before **each** process. The initial benchmark warmed only
once per workload group; that produced a systematic order effect in the saved
prototype. These results exclude earlier runs with that method or competing
build/test processes. All 54 ordered geometry fingerprints matched.

| Workload | Legacy CPU load (ms) | Before (ms) | Fixed prototype (ms) |
| --- | ---: | ---: | ---: |
| 200 small files | 132.6 | 124.9 | 97.2 |
| Mixed weighted polygons + rational curve | 651.1 | 105.0 | 68.1 |
| BusGameMap, 75.6 MB | 242.7 | 488.2 | 245.5 |
| Semantic3D, 10 million points | 1502.8 | 3590.8 | 1452.2 |
| Bennu, 815.9 MB triangles | 3495.6 | 6766.1 | 3446.3 |
| Powerplant, 817.9 MB with seams | 4420.9 | 7228.8 | 4510.0 |

Ordinary large-file CPU loading is 1.6 to 2.5 times faster than the saved
prototype. Its parsing phase is 3.5 to 4.8 times faster. Total load medians are
between 3.4% faster and 2.0% slower than legacy; these small differences should
be treated as comparable throughput, not a demonstrated improvement over legacy.
The point cloud still saves 229 MiB of peak process commitment (1187.0 to
958.0 MiB), with unchanged retained mesh-buffer capacities. The application
switch remains OFF by default, and the remaining issue 112 work listed above
is unchanged. Raw timings, phase measurements, memory counters and fingerprints
are in the generated local `build/prototype-fix-results.json` artifact.

The complete Debug application build and the Release comparison target built
without compiler warnings. With the prototype enabled, all 953 CTest checks
passed. The 16 focused parser cases passed 365,145 assertions, including every
64-byte block alignment for continued freeform bodies and earliest diagnostics,
file/memory/stream parity, numeric delimiters, statement limits, cancellation,
and overlapping read-only file handles.
