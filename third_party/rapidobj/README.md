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
scanner. Complete statements are batched into bounded text chunks before worker
dispatch; continuation lines cannot split a statement. Workers emit numeric
freeform records with physical line numbers and declaration counts. Woby resolves
their ordered state using its existing spline and trimming algorithms. There is
no parse-error retry, filtered polygon text, or second numeric parse.

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

The runner adds 200 small files and mixed weighted polygon/freeform fixtures,
rotates reader order, warms input, uses fresh processes, and requires matching
ordered geometry fingerprints. Windows reports peak working set and peak process
commitment. `parsed_chunk_bytes` counts live elements, not allocation capacities;
`peak_inflight_text_bytes` is a conservative bound on dispatched text, not total
process memory. The workload measures CPU loading, excluding GPU upload, first
visible frame and annotation preparation.

This prototype bounds in-flight source text, not all temporary geometry. Parsed
worker buffers still live until merge. It uses buffered I/O and a serial logical
scanner with parallel statement decoding; it does not yet reuse RapidOBJ's native
parallel file reader. Cancellation is checked while reading and collecting
bounded worker tasks, with joins before returning; merge and triangulation still
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

Ordinary large inputs take 2.8 to 4.2 times as long in this implementation.
Restoring their throughput is required before enabling the prototype by default;
the new scanner and dispatch path needs profiling and optimization. The result
supports modifying RapidOBJ for the grammar and ownership contract, but does not
justify replacing the current production loader yet. Small-file differences are
too small to treat as a meaningful speedup.

The local `build/prototype-results.json` artifact contains all raw timings,
prototype parse/freeform/preparation breakdowns, peak working set, peak process
commitment, retained capacities and fingerprints. It is generated output and is
not committed. GPU upload, first visible frame, annotation preparation and a
measured cancellation-latency bound remain outside this comparison.
