# Shared immutable geometry: implementation and performance

Idea 1 from [the simplification assessment](data-structure-algorithm-simplifications.md#geometry-ownership) is implemented. Scene files, comparison inputs, comparison results, and annotation work can share geometry buffers. The vertex order, source IDs, triangle order, and scene file format are unchanged.

## Ownership

`MeshGeometry` owns vertices, precise positions, triangle/line/point indices, mesh nodes, source hierarchy, and analysis batch IDs. `Mesh` holds a `shared_ptr<const MeshGeometry>` after publication. Bounds, coordinate origin, provenance handles, and derived cache handles remain in `Mesh`. Editable scene properties remain in `UiState`.

- `meshGeometry` gives read access. It cannot change the buffers.
- `editMeshGeometry` starts a separate mutable draft. For an existing published mesh, it copies the buffers, renews the content revision, and clears annotation and UV caches. Mutable references must not survive publication.
- `publishMeshGeometry` transfers the draft to the const owner. It does not copy the arrays. Loaders publish after their existing validation and finalization steps. Comparison preparation publishes after its geometry and normals are complete.
- A copy of a published `Mesh` shares its buffers. A copy of an unfinished draft still gets independent buffers. This prevents a builder from changing a worker's input through a retained mutable reference.

Comparison source results now share the prepared geometry. They do not retain unused import provenance or annotation caches. UV preparation finishes its mutable work before publication, so normal generation does not introduce a replacement copy.

Annotation preparation captures an owned input. Its cache owns the geometry directly. This removes the position-only mesh copy and the second cache wrapper. Geometry does not own a cache, so this graph has no ownership cycle. Document, file, revision, and buffer checks still reject stale worker results. File removal or replacement no longer requires a join for buffer safety.

History snapshots still contain metadata only. Undo preparation shares a live file's geometry. Restoration of a removed file still follows the existing reload policy.

## Measured latency

Measurements were taken on 10 October 2026. The baseline is commit `1d9fe69e1f7f74e4c000b7d60d16cfc45ff322e3`, with only the benchmark harness added. The post-change binary contains this implementation.

| Operation | Triangles | Before, ms | After, ms |
| --- | ---: | ---: | ---: |
| Snapshot one selected part | 100,000 | 3.595 | 0.00265 |
| Prepare undo | 100,000 | 3.610 | 0.01030 |
| Publish comparison source | 100,000 | 5.363 | 0.00180 |
| Prepare annotation cache | 100,000 | 20.983 | 14.755 |
| Snapshot one selected part | 1,000,000 | 34.641 | 0.00280 |
| Prepare undo | 1,000,000 | 34.913 | 0.01095 |
| Publish comparison source | 1,000,000 | 53.263 | 0.00180 |
| Prepare annotation cache | 1,000,000 | 226.988 | 148.915 |

Snapshot, undo, and source publication no longer scale with the buffer size in this workload. Annotation preparation still builds the spatial index and fingerprints. Its time fell by **34.4%** at one million triangles.

## Measured memory

The following values are the largest observed increase in process private bytes while one result is held and the source remains live. They are process measurements, not sums of vector sizes. Values below 0.05 MiB do not mean that the operation makes no allocations.

| Operation, one million triangles | Before, MiB | After, MiB |
| --- | ---: | ---: |
| Snapshot one selected part | 172.01 | <0.05 |
| Prepare undo | 172.43 | <0.05 |
| Publish comparison source | 172.00 | <0.05 |
| Prepare annotation cache | 108.83 | 6.05 |

Across the complete benchmark process, the median sampled peak private memory fell from **347.06 MiB to 194.00 MiB**, a **44.1% reduction**. The median peak working set fell from **354.45 MiB to 202.02 MiB**. These peaks include fixture construction and all four operations. They are not isolated peaks for each operation.

The one-million-triangle fixture has three million render vertices. Its vertex, precise-position, and triangle-index payload is 180,000,000 bytes, or 171.66 MiB. This explains the removed snapshot copies. The old annotation snapshot added 108,000,000 bytes before its cache data.

## Method and limits

The machine used Windows 11 Home 10.0.26300, an AMD Ryzen 7 5800H with 8 cores and 16 logical processors, and approximately 64 GiB RAM. Both binaries used the `vs2026-vcpkg` preset, x64 Release, and MSVC 14.51.36231.

The [C++ workload](../tests/geometry_storage_benchmarks.cpp) creates deterministic triangles with separate corner vertices and precise positions. Each file has two parts. Only the first triangle is selected for the snapshot measurement. The comparison-source and annotation measurements use the complete mesh. Import, world-space transformation, and fixture construction are outside the operation timings.

There were three processes per variant. Their order was before/after, after/before, before/after. Each operation had one warm-up and eight timed calls per process. The table reports the median of the three process medians. The timer stops while the result is still alive; destruction is excluded. There were no concurrent builds or tests during measurement.

The [runner](../tests/run_geometry_storage_benchmarks.ps1) checks all returned count checksums. It samples process private memory at 10 ms intervals and reads the operating system's peak working-set counter. Short private-memory peaks can fall between samples. Operation memory deltas include allocator behavior and use all nine calls.

The [raw timing and retained-memory data](shared-geometry-results.csv) and [process memory, machine details, and binary hashes](shared-geometry-memory.json) are retained with this report.

These are CPU ownership measurements. They do not establish an FPS, GPU-memory, import, or detector-algorithm speedup. Metadata still gets copied; files with many parts can have a larger metadata cost. Starting an edit of published geometry still requires a replacement buffer set.

An annotation job that outlives file removal now retains the complete geometry owner, including precise positions. The previous annotation snapshot retained fewer fields. Memory during that interval can therefore increase. The owner is released when its last consumer is released. Tests check release and the absence of cache cycles; history does not retain removed geometry.

## Validation and reproduction

The full Debug build completed without compiler warnings. All **107 CTest entries** were validated, including 25 graphics entries and 8 entries with the slow label. The full run passed 103 entries. Three old ownership assertions and one benchmark's mutable-buffer move needed updates; all four affected entries passed the recheck. Seven new ownership test cases, with 50 assertions, passed in both Debug and Release. The Release build also completed without compiler warnings.

Coverage includes shared buffer identity, draft isolation, mixed primitives, hierarchy and batch storage, precise coordinates and signed zero, annotation cache release, removal/replacement, file-vector relocation, late result rejection, unchanged-buffer revision changes, and history ownership. Existing import, analysis, annotation, undo/redo, save/load, picking, and rendering checks also passed.

```powershell
cmake --preset vs2026-vcpkg -DWOBY_TEST_HEADLESS=ON `
    -DWOBY_BUILD_BENCHMARKS=ON -DWOBY_BUILD_HASH_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2
cmake --build --preset vs2026-vcpkg --config Release --target woby_tests
```

For a new baseline build, create a detached worktree at the baseline commit under `D:\.worktree`. Copy `tests/geometry_storage_benchmarks.cpp` into it and add `target_sources(woby_tests PRIVATE tests/geometry_storage_benchmarks.cpp)` after the test target is defined. The harness supports both storage APIs. Configure and build that checkout with the same preset and Release configuration.

Then run from the implementation checkout:

```powershell
.\tests\run_geometry_storage_benchmarks.ps1 `
    -BeforeExecutable D:\.worktree\woby-geometry-before\build\vs2026-vcpkg\bin\Release\woby_tests.exe `
    -AfterExecutable .\build\vs2026-vcpkg\bin\Release\woby_tests.exe `
    -OutputDirectory .\build\geometry-measurements -Runs 3
```
