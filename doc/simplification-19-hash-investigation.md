# Item 19: analysis index and obsolete parser copy

Decision: keep the specialized insertion-ordered analysis index. The map adapters raise reserved memory and do not improve the complete current workloads. The separate single-key dense set also regresses the default detector batches and leaves aggregate index peak memory unchanged. A container substitution does not remove the indexing transformation. The parser-position copy in the original warning is already absent.

This investigation applies to commit `46a27585c5bc8638a8bde5d601e5cead71376631` on 11 October 2026. Working item 19 covers the hash and parser-copy clauses in the final unnumbered paragraph. Item 18 covers the separate broad geometry-library proposal.

## Index contract and experiment

[`AnalysisIndex`](../src/analysis_index.h) stores each key once in first-insertion order and keeps bucket entries as indices into that vector. Detector consumers use the vector and its stable IDs. Coordinate hashing treats positive and negative zero as equal. Replacing the table with an ordinary key/value map still needs to retain this order and identity.

The existing [benchmark adapter](../tests/benchmark_analysis_index.h) uses the same production hash and a parallel ordered key vector with Boost.Unordered or unordered_dense. It duplicates keys between that vector and the map. This duplication belongs to these adapters; it is not a proof that every possible library adapter needs duplicate keys. A specialized interface over unordered_dense's insertion-ordered values could avoid some duplication, but would need to prove ID stability and the current key-vector contract.

The [new microbenchmark](../experiments/simplification-19/main.cpp) tests 4,096 and 524,288 coordinate and integer triple keys, shuffled with seed 417. Each sample reserves the table, inserts all keys and performs three successful-hit passes. It excludes destruction and input construction. All routes warm up. The order rotates among custom, Boost and unordered_dense for nine samples when the optional hash dependencies are enabled; a two-route build alternates custom and Boost. Checksums, cardinality and ordered keys agree. The self-test performs actual signed-zero insertions into every enabled index.

Each library allocator and its ordered vector share one byte recorder. Retained and peak figures are exact requested payload for those owners, including map metadata; they exclude allocator headers, process memory and input keys. The custom route uses its reserved vector capacities. Reserve prevents insertion growth for this workload. The raw records include any initial map allocation and replacement peak.

The existing detector executables also run unchanged current detector algorithms with only their index adapter changed. BusGameMap and Powerplant each use two fresh processes per backend, three samples per process, with reversed backend order on the second pass. Import, source preparation before the timer, rendering and GPU work are excluded. The driver requests the stage batches returned by `nextComparisonStage`, including its combined automatic-detector batch, and sums their time. It does not model the application's warm runtime cache. The runner preserves every trailing detector count and validates full vectors across all runs and backends before writing results. Counts alone do not prove complete finding/order equality, so production adoption also needs that regression coverage.

See [raw results and provenance](simplification-19-results.json), [raw detector CSV files](benchmarks/simplification-19), and [the runner](../experiments/simplification-19/run.ps1). The previous [hash-table investigation](hash-table-performance.md) uses older code and a different expanded-input workload; its medians are historical evidence rather than a current baseline.

## Current map-adapter results

Microbenchmark medians below are milliseconds for one insertion pass and three hit passes, over nine rotated samples. They include reserve and exclude destruction.

| Key family; entries | Current index | Boost map plus ordered keys | unordered_dense map plus ordered keys |
| --- | ---: | ---: | ---: |
| Double triples; 4,096 | 0.577 | 0.715 | 0.709 |
| Double triples; 524,288 | 211.321 | 207.602 | 179.232 |
| Integer triples; 4,096 | 0.558 | 0.559 | 0.455 |
| Integer triples; 524,288 | 204.516 | 196.561 | 148.107 |

The large unordered_dense map adapter is 15.2% faster for coordinate keys and 27.6% faster for integer keys. Its coordinate range is 160.47–196.26 ms, versus 195.89–235.74 ms for current code; integer ranges are 141.87–153.51 and 196.83–220.08 ms. Boost's large coordinate ranges overlap substantially, with a 304.97 ms sample retained in the data. Small coordinate medians regress with both adapters. This is not a universal lookup gain.

Both key types are 24-byte triples on this x64 host, and their reserved memory is identical:

| Entries | Current key/bucket capacity | Boost adapter requested retained/peak | unordered_dense adapter requested retained/peak |
| ---: | ---: | ---: | ---: |
| 4,096 | 160 KiB | 344 KiB | 288 KiB |
| 524,288 | 20 MiB | 43 MiB | 36 MiB |

At the large size, the tested adapters retain 115% and 80% more index storage. The current index is measured by its two vector capacities; the adapter recorder covers both map storage and the ordered vector. These figures apply to this reserved index, not all application memory or a library set that stores each key once.

The complete current detector batches do not show a useful unordered_dense map gain. The table uses six total-time samples per backend and model, across two fresh processes. Values are median milliseconds with full ranges.

| Model | Current | Boost adapter | unordered_dense map adapter |
| --- | ---: | ---: | ---: |
| BusGameMap | 1433.70 (1381.78–1630.11) | 1560.25 (1489.80–1647.09) | 1428.63 (1396.10–1523.21) |
| Powerplant | 18309.75 (18178.70–18347.30) | 20415.80 (19728.80–21340.50) | 18420.75 (18380.00–20347.90) |

Boost is 8.8% and 11.5% slower at the medians. The unordered_dense map adapter changes them by -0.35% and +0.61%; Bus ranges overlap. Its faster standalone lookup does not establish a pipeline gain. All nine detector-count fields match across all 36 real-model samples; the runner also verifies input and stage layout. The nine-count vectors are retained in the raw JSON and CSV files, including stage rows with no counts. Complete finding/order equality remains a production gate.

These tested map adapters are not justified for a general substitution: they raise reserved index memory and fail to improve the complete workloads. The [separate dense-set follow-up](simplification-19-dense-set-investigation.md) supplies the read-only key interface directly from insertion-ordered set values. It stores each retained key once and validates growth/copy/move/signed-zero ownership. Its matched instrumented default pipeline is 7.34% slower on BusGameMap and 2.95% slower on Powerplant, with no useful aggregate index-peak saving. Powerplant allocation traffic falls by 8.80%; this is not a peak-memory reduction. Its find-first variant has only small, overlapping large-micro gains and no measured pipeline gain. The report records the 32-bit domain/fallback gate and the insertion/hit mix of the current sources. These results support retaining the current general index without ruling out every possible library adapter or a targeted hit-heavy workload.

## Parser copy audit

[`obj_mesh.cpp`](../src/obj_mesh.cpp) passes caller-owned double position storage to the parser, moves those positions into `SourceMeshData`, and assigns a shared owner to `MeshGeometry::sourcePositions`. These steps are already present. A proposal to remove another whole double-position copy from this path has **zero additional savings** at this commit.

This does not imply import has no copies. Tuple flattening, attributes, triangulation, freeform data and position maps have their own costs. [Item 14](simplification-14-obj-investigation.md) measures a specific alternative and preserves this already-improved source-position ownership.

## Merge boundary

For a container change, require equal keys, first-use IDs, signed-zero handling, complete findings, order, topology modes, cancellation and bounded-result status. Measure cold automatic analysis and shared-cache analysis separately. Record peak and retained application memory on large inputs, rather than projecting the microbenchmark's allocation ratio onto all graph storage.

Keep source identities and explicit provenance outside container iteration order. Do not substitute the OBJ tuple table on the basis of this detector result. No broad replacement or parser-copy deletion is ready to merge from this investigation.
