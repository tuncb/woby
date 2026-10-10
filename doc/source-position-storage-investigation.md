# Source position storage investigation

Item 2 can remove one double-precision coordinate copy per render vertex. Start with identity indexing for importer output. For OBJ, retain the source position ID when the loader creates each render vertex. Keep direct position storage for transformed comparison and UV meshes.

The memory benefit is clear, but indirect reads have a cost. In the representation probe, identity indexing had approximately the same read time as copied positions. At four million render vertices, shuffled source IDs made a sequential scan 6.05 times slower. This change should be treated as a memory optimization, with application benchmarks required before broad adoption.

This investigation uses commit `d8eaa74`, after item 1, on 10 October 2026. This is the pre-implementation investigation. See the [implementation and validation report](source-position-storage-implementation.md) for the completed change. The [standalone probe](../experiments/source_position_storage.cpp) compares coordinate representations. It does not measure a modified loader or a modified `meshPosition` function.

The follow-up [BearTrap and BusGameMap application benchmarks](source-position-model-performance.md) use a mapped-position prototype with the actual comparison, dimension, and bounds functions. Comparison preparation was about 5-6% slower; the other measured operations changed much less. Those workloads did not reproduce the synthetic 2x random-read slowdown. The report gives sampling and memory limits.

## Current position paths

| Path | Current behavior | Required change |
| --- | --- | --- |
| OBJ faces | Each new position/normal/UV tuple copies its source double coordinate into `precisePositions`. | Append the known position ID instead. Preserve tuple order and render indices. |
| OBJ lines and points | `primitiveVertex` also copies the source coordinate. Vertex-only clouds visit all source points. | Retain the ID here too. Do not infer identity from the absence of UVs or normals. First-use order can differ from source order. |
| Importer output | Read doubles, validate point identities, localize, rebase, then copy the positions in `captureSourceMesh`. | Transfer the finalized position buffer into a shared owner. Render and source array indices are identical. |
| Freeform append | Append each evaluated point to both precise and source arrays. The two arrays can have different starting offsets. | Append one coordinate to the source owner and map the new render vertex to `sourceOffset + sampleIndex`. |
| World comparison and UV layout | Build new coordinates after a transform or UV projection. | Retain direct positions. Original source coordinates are not these output coordinates. |

Sources: [OBJ construction](../src/obj_mesh.cpp), [importer construction](../src/importer_host.cpp), [source capture and coordinate frames](../src/model_mesh.cpp), [freeform append](../src/freeform.cpp), [comparison construction](../src/comparison_scene.cpp).

`meshPosition` is the main precise-coordinate access boundary. Its callers include comparison assembly, distance and surface analysis, UV quality, dimensions, and some picking paths. Only five production `.cpp` files access `precisePositions` directly: `model_mesh`, `obj_mesh`, `importer_host`, `freeform`, and `comparison_scene`. The last file also hashes the precise-position count in comparison signatures. The migration must replace that representation-dependent signature input while preserving content-revision invalidation.

GPU vertices stay at 32 bytes. Normal generation, annotation geometry, and point-cloud construction use float render positions in relevant paths. Removing the double copy does not reduce GPU vertex upload size or directly reduce those float-coordinate calculations.

## Position ownership

The coordinate owner must belong to `MeshGeometry`. Do not implement lookup by reading only `Mesh::sourceData`. Both [comparison source results](../src/mesh_comparison.cpp) and [annotation snapshots](../src/annotation_work.cpp) clear that metadata. The geometry must remain valid after those operations and after removal of the source file.

Use a small immutable position owner, shared by geometry and source data. It should contain coordinates only. Keep source triangle indices, importer identities, and provenance in `SourceMeshData`. An aliasing pointer into the current `SourceMeshData::points` vector would be a small first patch, but it would retain the whole source object, including its indices and identities, for the lifetime of every geometry consumer.

Three explicit access modes are sufficient:

| Mode | Lookup | Use |
| --- | --- | --- |
| Render float | Promote `vertices[i].position` to doubles | Existing small generated meshes that have no precise array |
| Direct double | `positionOwner.points[i]` | Identity importer output and direct transformed output |
| Indexed double | `positionOwner.points[renderToPosition[i]]` | OBJ tuples, and mixed geometry with different source/render offsets |

Direct mode is an index relationship, not a statement that source provenance exists. A transformed mesh can own direct coordinates without a source record. An empty mapping must not serve as an ambiguous signal for both missing positions and identity indexing.

Validate the relationship before publication: the direct owner must cover every render vertex; an indexed map must contain one ID per render vertex; every ID must be within the coordinate array. Preserve current bounds checks and source count limits. OBJ already limits source positions to signed 32-bit range; freeform append checks unsigned 32-bit counts. Do not narrow importer `originalPointIds`, which are 64-bit identities rather than coordinate offsets.

The nested position owner also needs an edit rule. `editMeshGeometry` currently copies vectors when it detaches a published mesh. After this change, copying a geometry wrapper must not allow modification of shared coordinates. A position edit must create a new owner and publish matching geometry and source metadata together. Rebase each source position once, including unused positions; rebuild render floats from the resulting coordinates. Do not reconstruct precise positions from floats.

## Identity and compatibility

Coordinate equality is not source identity. Two OBJ `v` records with equal coordinates must keep different source IDs. Different UV/normal tuples that use one source position must keep their separate render vertices. Preserve all unused source points. The original-index topology mode operates on source array indices; exact-position topology performs its own grouping.

Importer `originalPointIds` have a separate role: duplicate-point counting uses them, while connectivity and reported source indices retain their current meaning. Identity position storage must not merge importer vertices with matching authored IDs. See [source data](../src/mesh_duplicates.h), [duplicate inspection](../src/mesh_duplicates.cpp), and [topology construction](../src/mesh_topology.cpp).

Freeform append needs an explicit transition. An identity prefix can remain direct only if the next render offset equals the next source offset. If unused OBJ positions or seams make the offsets differ, create the mapping for the existing prefix and append the correct source IDs. Preserve the source triangle order and generated point identities. Keep the current sequence of localization and rebase operations, including rounding and signed-zero behavior.

Point marker IDs are not source position IDs. [Scene mesh preparation](../src/scene_mesh_preparation.cpp) emits each render vertex once per group. [Point clouds](../src/point_cloud.cpp) use those display occurrences to assign marker IDs and then change spatial order. Several markers can refer to the same source position. Replacing marker IDs with source IDs would change selection and display behavior. Keep marker ordering and the existing render-vertex relationship in the first implementation.

## Memory calculation

Let `R` be the render vertex count and `S` the source position count. While the live mesh retains source data, the relevant array payloads are:

| Representation | Render vertices plus double coordinates and mapping |
| --- | --- |
| Current | `56R + 24S` bytes |
| Explicit source mapping | `36R + 24S` bytes |
| Identity source mapping | `32R + 24S` bytes |

These calculations exclude triangle and primitive indices, vector capacity, allocator overhead, and derived caches. Those structures do not change in this proposal. Shared snapshots from item 1 already avoid copies, so count this saving once per geometry owner, not once per snapshot.

| Render vertices | Explicit mapping saving | Identity saving |
| --- | --- | --- |
| 1,000,000 | 19.07 MiB | 22.89 MiB |
| 4,000,000 | 76.29 MiB | 91.55 MiB |
| 10,000,000 | 190.73 MiB | 228.88 MiB |

Lifetime can reverse the saving. After source metadata is released, a current geometry-only consumer retains `24R` coordinate bytes. A mapped consumer must retain `24S + 4R`. Its coordinate payload is smaller only when `S < 5R/6`. For one million rendered positions chosen from four million source positions, the retained coordinate payload grows from 22.89 MiB to 95.37 MiB. A coordinate-only owner avoids retaining source indices and authored IDs, but it still retains unused coordinates. Keep direct transformed outputs and measure source removal with long-lived workers/results. Do not compact or drop unused source points as a hidden fix.

## Read measurements

The probe uses one million and four million render vertices. Each fixture has the current copied coordinates and the candidate source relationship. It checks all coordinate bits, including signed zero, before timing. Equal coordinates at separate source indices are retained. Each read consumes all three coordinates; both variants must produce the same ordered checksum.

Fixtures use these relationships:

- **Identity:** one source point per render vertex, in the same order.
- **Local seams:** four adjacent render vertices per source point.
- **Shuffled seams:** the same four-to-one relationship, with a deterministic permutation of the mapping.
- **Unused positions:** one million render vertices select one in four source points, then permute the mapping.

Sequential reads visit each render index in order. Random reads visit a deterministic permutation of all render indices. These are position reads, not full mesh analysis. The probe uses bounds-checked vector access and specializes each representation before entering the loop; production mode dispatch and `meshPosition` call overhead are not measured.

Each of three fresh processes performs one warm-up pair and nine measured pairs. Execution order alternates within each pair sequence and changes with the process number. The table uses the median of the three process medians. Both representations are resident during measurement. Caches are not flushed. There are no concurrent builds or tests during the measured runs. The CSV payload columns describe the proposed representations; they exclude the probe's extra oracle, mapping-generation, and query arrays.

| Relationship | Render vertices | Access | Copied ms | Candidate ms | Candidate / copied |
| --- | --- | --- | --- | --- | --- |
| Identity | 1,000,000 | Sequential | 1.73 | 1.76 | 1.02 |
| Identity | 1,000,000 | Random | 7.63 | 7.47 | 0.98 |
| Local seams | 1,000,000 | Sequential | 1.73 | 1.73 | 1.00 |
| Local seams | 1,000,000 | Random | 7.72 | 16.12 | 2.09 |
| Shuffled seams | 1,000,000 | Sequential | 1.78 | 7.63 | 4.28 |
| Shuffled seams | 1,000,000 | Random | 7.74 | 14.79 | 1.91 |
| Unused positions | 1,000,000 | Sequential | 1.76 | 12.62 | 7.18 |
| Unused positions | 1,000,000 | Random | 7.70 | 19.85 | 2.58 |
| Identity | 4,000,000 | Sequential | 6.90 | 7.15 | 1.04 |
| Identity | 4,000,000 | Random | 40.16 | 40.32 | 1.00 |
| Local seams | 4,000,000 | Sequential | 7.11 | 6.99 | 0.98 |
| Local seams | 4,000,000 | Random | 40.18 | 81.08 | 2.02 |
| Shuffled seams | 4,000,000 | Sequential | 6.90 | 41.73 | 6.05 |
| Shuffled seams | 4,000,000 | Random | 40.25 | 81.62 | 2.03 |

Test host: AMD Ryzen 7 5800H, 8 cores / 16 threads, approximately 64 GiB RAM, Windows 11. Toolset: MSVC 14.51.36231, x64, `/O2 /MD /std:c++20 /EHsc /W4 /WX /fp:precise`. The [raw results](source-position-storage-results.csv) contain all 84 rows, including per-process minimum and maximum times. The [metadata](source-position-storage-metadata.json) records the source and executable hashes.

The extra dependent read and source order explain a plausible cause of the mapped-read regressions; hardware counters were not collected. Identity reads access the same coordinate layout as the copied representation, and the small timing differences do not establish an application speedup or slowdown. These runs do not measure load time, construction peak memory, process memory, detector completion, picking latency, or FPS. Memory figures above are representation calculations.

## Implementation sequence and acceptance checks

1. Introduce the immutable coordinate owner and explicit access modes behind `meshPosition`. Keep direct generated geometry and the float fallback. Migrate source readers without changing source IDs or topology.
2. Move finalized importer coordinates into the shared owner. Keep its source triangle index buffer unchanged. Verify position identity, importer point IDs, freeform append, large-coordinate localization, and rebase.
3. Retain IDs in both OBJ vertex creation paths. Preserve first-use tuple order, lines, points, seams, unused positions, and negative-index resolution. Publish the map with its coordinate owner.
4. Update signature inputs, edit detachment, source capture, and annotation/result ownership. Check coordinate lookup after source metadata reset, source removal, undo/reload, and late worker completion.
5. Measure full import, analysis preparation, repeated precise queries, and memory before accepting mapped storage as the default for OBJ. Use both the existing large-file corpus and deliberately poor source order. Keep direct coordinates in transformed analysis results; add a derived direct-access cache only if measured query cost justifies its memory.

Add unit tests for exact coordinate bits and source IDs, not only equal bounds or finding counts. Exercise an identity-to-mapped freeform transition, an invalid mapping, copy/edit isolation, and owner release. Compare ordered triangles, source points, render vertices, normals, UVs, marker IDs, and all topology/duplicate findings. Existing coverage in [OBJ tests](../tests/obj_mesh_tests.cpp), [importer identity tests](../tests/importer_point_ids_tests.cpp), [coordinate-origin tests](../tests/coordinate_origin_tests.cpp), [freeform tests](../tests/freeform_tests.cpp), and [shared ownership tests](../tests/shared_geometry_tests.cpp) supplies the baseline fixtures.

For application timings, compare the committed item 1 baseline against item 2 with the same Release configuration and input hashes. Alternate complete runs and preserve complete results. Record retained memory both while the source file is live and after it is removed. Keep transient construction memory separate from the final payload saving.

## Reproduction and validation

From an x64 Visual Studio 2026 developer PowerShell at the repository root:

```powershell
./experiments/run_source_position_storage.ps1 -Runs 3
```

The runner builds the standalone probe with warnings treated as errors and writes `results.csv` and `metadata.json` under `build/source-position-storage`. It requires no external model files and creates no temporary scene fixtures. The application and its unit tests use the normal project preset:

```powershell
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2
```

Validation completed on 10 October 2026: the Debug application build passed without warnings, and all 107 CTest entries passed in 356.66 seconds, including the graphics and slow suites. The standalone probe compiled with `/W4 /WX`. The runner completed three fresh processes and saved all 84 expected result rows. All bit comparisons and read checksums passed. This validates the current application and the probe; production item 2 compatibility remains subject to the implementation checks above.
