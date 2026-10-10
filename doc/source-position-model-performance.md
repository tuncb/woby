# Source position access on large OBJ models

This experiment compares the current precise-position array with explicit source-position mapping in the application functions. It uses the requested BearTrap terrain model and the smaller BusGameMap model. These are historical prototype results, recorded before the production implementation. See the [production comparison](source-position-storage-implementation.md) for the completed change and its measurements.

The [earlier synthetic probe](source-position-storage-investigation.md) found large regressions with random access and shuffled source IDs. Application operations include other work, and their access order can be more local. These measurements test that difference.

## Workloads

| Operation | Application function | Work included |
| --- | --- | --- |
| Group setup | `createUiGroupStates` | Create settings, calculate float local bounds and precise original bounds for every group |
| Group bounds | `originalMeshBounds(mesh, node)` | Calculate precise original bounds for every group, without using cached group bounds |
| Dimensions | `sceneDimensions` | Recalculate dimensions with all triangle groups selected; cache hits are excluded |
| Comparison preparation | `comparisonWorldMesh` | Build the complete world-coordinate comparison input, including remapping, vertices, precise positions, indices, batches, nodes, provenance, and bounds |

The comparison uses the complete model with default group transforms. Derived comparison meshes keep direct precise positions. Detector execution, GPU uploads, rendering, and frame rate are outside these timings.

Point and line picking is applicable only to native point and line primitives. The runner checks the primitive buffers. It does not turn triangles into synthetic point or line workloads.

## Prototype and validation

The [benchmark targets](../experiments/position-access/CMakeLists.txt) link the normal `woby_core`. The `before` executable uses the production position accessor. The `mapped` executable compiles a generated copy of `model_mesh.cpp` with a lookup branch for the benchmark's imported geometry. Every other geometry owner uses the normal accessor. The prototype keeps the source owner and map alive for the synchronous process. It is not a complete implementation of source editing, rebase, or publication.

The loader is unchanged in both executables. After loading, the runner derives the map from corresponding entries in the render and source triangle buffers. Every occurrence of a render vertex must have the same source ID. Every mapped coordinate must match the current precise coordinate bit for bit. The runner rejects unsupported geometry and missing relationships; it never reconstructs IDs by comparing coordinates.

The mapped variant then removes the copied precise-position array through the geometry edit API. It preserves the capacities of all other geometry vectors. Map construction, validation, detachment, and input hashing are outside operation timings. This is an operation benchmark, not an import-speed or construction-peak benchmark. The final report excludes the initial pilot, whose mapped conversion could compact unrelated vector capacities.

Each operation has one warm-up. Its output stays alive until timing stops. Output destruction, process-memory reads, and result checks occur outside the measured interval. Every warm-up and measured call must produce the same result within its process. The runner also compares both variants and all processes. It checks exact dimensions and group bounds, and ordered fingerprints of all output vertex, precise-position, index, and batch arrays, plus node names, primitive ranges, source object IDs, UV availability, bounds, and origins. Input source arrays and the complete source mapping also have ordered fingerprints.

The contract tests cover seam-expanded vertices, equal coordinates with different source IDs, unused source positions, source metadata removal, geometry copies, invalid lookup indices, and the direct-position fallback. The fixture is an in-memory OBJ and creates no filesystem files.

## Results

The application operations did not reproduce the synthetic 2x slowdown on either model. Comparison preparation increased by about 5-6%. Bounds and group setup changed little. The one BearTrap dimensions sample was faster with mapping; that single result does not establish a speedup.

Measurements used an AMD Ryzen 7 5800H with 8 cores / 16 threads, approximately 64 GiB RAM, Windows 11, and MSVC toolset 14.51.36231 (`cl` 19.51.36243.0), x64 Release with the `vs2026-vcpkg` preset. The baseline is commit `d8eaa74`. The [raw results](source-position-model-results.json) include every sample, result fingerprints, input SHA-256 hashes, executable hashes, and prototype source hashes.

| Model | File bytes | Render vertices | Source positions | Triangles | Groups |
| --- | --- | --- | --- | --- | --- |
| BearTrap | 7,108,991,827 | 38,414,391 | 37,890,645 | 75,771,986 | 1 |
| BusGameMap | 75,585,857 | 547,469 | 535,893 | 1,054,542 | 65 |

Both files contain triangles only. **Native point/line picking is not applicable to either file.** Neither test measures that operation.

BearTrap has one process per variant, with one warm-up and one measured call per operation. Its initial baseline pilot took 98 seconds, so the repeated-run campaign used the smaller model supplied by the user. The table below uses the final capacity-preserving prototype only. BusGameMap has three processes per variant, with one warm-up and three measured calls per operation in each process. BusGameMap values are medians of the three process medians.

| Model | Operation | Before ms | Mapped ms | Change |
| --- | --- | --- | --- | --- |
| BearTrap | Group setup | 7,566.55 | 7,528.47 | -0.50% |
| BearTrap | Group bounds | 5,806.87 | 5,833.59 | +0.46% |
| BearTrap | Dimensions, uncached | 6,021.70 | 5,775.08 | -4.10% |
| BearTrap | Comparison preparation | 6,934.80 | 7,306.64 | +5.36% |
| BusGameMap | Group setup | 96.81 | 95.68 | -1.16% |
| BusGameMap | Group bounds | 75.35 | 75.42 | +0.09% |
| BusGameMap | Dimensions, uncached | 84.08 | 84.69 | +0.72% |
| BusGameMap | Comparison preparation | 78.68 | 83.32 | +5.90% |

For BusGameMap comparison preparation, all nine before samples were 77.58-79.87 ms; all nine mapped samples were 81.37-83.70 ms. The separation supports a modest regression in this workload. The other operation ranges overlap. BearTrap comparison preparation increased by 372 ms in its single measured pair. More fresh processes are needed to establish its stable percentage.

### Retained memory

| Model | Position array capacity before MiB | Position array capacity mapped MiB | Input process private memory before MiB | Input process private memory mapped MiB |
| --- | --- | --- | --- | --- |
| BearTrap | 1,300.87 | 146.54 | 5,651.17 | 4,494.50 |
| BusGameMap | 18.40 | 2.09 | 83.29 | 66.82 |

Source coordinates remain resident in both variants. The position-capacity columns count the extra precise array in the baseline and the ID array in the mapped variant. Other render-vector capacities match between variants. Process values are sampled after input conversion and validation, before operation results are created; BusGameMap values are medians across its three processes.

The logical saving is **732.70 MiB for BearTrap** and **10.44 MiB for BusGameMap**, at 20 bytes per render vertex. The measured position-capacity saving is larger: **1,154.33 MiB** and **16.31 MiB**. The old precise arrays had unused capacity from vector growth, while the reconstructed maps have exactly one entry per render vertex. A future loader must reserve or grow its ID arrays deliberately to obtain the same capacity saving.

### Why the application result differs from the random read probe

BearTrap has only about 1.4% more render vertices than source positions. BusGameMap has about 2.2% more. The synthetic seam fixtures had four render vertices per source point and included fully shuffled relationships. On BearTrap, 78.5% of adjacent render vertices map to source indices at most 16 positions apart. This suggests more useful locality, but no hardware cache counters were collected.

These functions also perform substantial work beyond coordinate reads. Comparison preparation examines every triangle corner for remapping, but fetches a precise coordinate only when it creates an output vertex. It then writes several output buffers and calculates bounds. Bounds and dimension scans include other loads, finite checks, coordinate calculations, and minimum/maximum updates. Doubling the cost of an isolated read loop would not double all that work.

The evidence supports keeping the proposed direct storage for transformed comparison results. An OBJ mapping offers substantial retained-memory savings here at a modest comparison-preparation cost. It does not yet justify a claim about total import time, detector execution, picking, or FPS.

## Interpretation limits

The sampled process peak includes loading and the prototype conversion. It must not be reported as the peak memory of an implemented mapped loader. The coordinate-capacity measurements describe the retained representation in this experiment. A future loader could use different vector capacities. Input memory and operation-result memory are separate from the logical saving of 20 bytes per mapped render vertex.

The prototype selects mapped access through a benchmark-local geometry registration. The final implementation needs its own measurements because its coordinate ownership and access-mode dispatch will differ.

These are repeated CPU operations on already loaded models. File hashing occurs before the runs and can warm the file cache; loader duration is recorded only as setup information. Caches are not forcibly flushed. No builds or tests run concurrently with the measurements. Small percentage differences, especially in the limited BearTrap sample, do not establish a stable application regression or speedup.

## Reproduction status

These historical runs used the benchmark-only `before` and `mapped` targets. Their full results and executable/source fingerprints are retained in [the raw record](source-position-model-results.json). The production implementation replaces the prototype target with `woby_position_production`. The [current reproduction instructions](source-position-storage-implementation.md) compare that target with the saved pre-change executable; the commands for the old prototype no longer apply to this checkout.

## Build and test results

Both Release benchmark targets compiled without warnings. The full Debug build also completed without warnings. All **109 CTest entries passed** in 357.42 seconds, including both new position-access contract tests, the graphics tests, and the slow tests. All benchmark output comparisons passed across the eight final processes. No production position-storage implementation was changed.
