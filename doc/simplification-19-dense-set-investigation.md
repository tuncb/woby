# Item 19 follow-up: one retained key per dense set

Decision: keep the current general analysis index. The tested dense set preserves first-use IDs and removes the duplicate key vector used by the earlier map adapters. It makes the matched instrumented default detector batches 7.34% slower on BusGameMap and 2.95% slower on Powerplant. The measured index allocation peak is unchanged within 32 bytes. Powerplant has lower allocation traffic, but this is not a peak-memory saving. The separate find-first micro results do not justify a general substitution.

This is a separate campaign from the [map-adapter investigation](simplification-19-hash-investigation.md). It uses commit `46a27585c5bc8638a8bde5d601e5cead71376631`, MSVC x64 Release, and an AMD Ryzen 7 5800H with 8 physical cores and 16 logical processors. The exclusive serial timing window was 00:18:08–00:22:57 UTC on 11 October 2026. No production optimization is merged or enabled by these probes.

## Adapter and domain

The local unordered_dense 4.8.1 set exposes its insertion-ordered contiguous `values()`. The [adapter](../experiments/simplification-19-dense-set/dense_set_index.h) uses those values as the retained keys. It has no parallel ordered vector. It calls the unchanged production `analysisKeyHash`, keeps the library's normal hash mix, and does not declare `is_avalanching`.

The three substituted production modules are `mesh_comparison.cpp`, `mesh_duplicates.cpp`, and `mesh_topology.cpp`. Their key consumers use only `keys.size()` and `keys[id]`. They do not erase or mutate retained keys. The owning read-only facade derives each accessor from its own set. It stores no pointer or span that needs rebinding after copy or move. Erase is not exposed through the facade: library erase moves the last value and would change IDs. The lookup owner remains public, as required by the procedural code style; callers must use the free insertion and reserve functions.

Set insertion appends before it checks for a duplicate. A full value vector can therefore grow on a hit. The measured default strategy reserves `N+1`, keeps a spare after unique growth, and computes the returned ID before a reserve can invalidate the iterator. Growth is geometric. A full-capacity owner searches first; this covers copies whose value vector has capacity equal to size. Hits keep the key address and do not request an index allocation. Unique insertion and reserve can invalidate borrowed key references, as with the current vector.

The standard bucket is 8 bytes and has a 32-bit value index. On this host, library `max_size()` is `2^32`. The probe rejects reserve requests and new unique counts beyond `2^32-1` before the library operation. It preserves hits at the limit. This does **not** preserve the full production `size_t` domain. A production port must keep the current wide index as a fallback or establish a wide-bucket route. The micro also tests the packed 12-byte big bucket. Its probe limit is the inherited `SIZE_MAX >> 2`, or `2^62-1` on this host. These are source-level capacity guards, not experiments with billions of keys.

Floating keys must satisfy the existing finite-input boundary. Signed zeros compare and hash alike. The first inserted representative keeps its original bits. `importer_host.cpp` is outside the three-module pipeline substitution and remains an integration gate for a general header replacement.

## Measurement and validation

The [runner](../experiments/simplification-19-dense-set/run.ps1), [source extraction](../experiments/simplification-19-dense-set/CMakeLists.txt), [raw results](simplification-19-dense-set-results.json), and [derived summary](benchmarks/simplification-19-dense-set/summary.json) preserve the experiment. Generated copies differ from production in one checked include per module. The custom baseline uses the production index header with checked allocator/type-name substitutions; its hash and probing bodies are unchanged. Source changes are CMake configure dependencies. Before and after the run, the runner validates these copies and hashes the production inputs, generated files, dependency header, project files, and all three executables.

Both custom and set routes use the same tracking allocator. It counts only successful index allocation requests and their deallocation, including set bucket rebinds. It does not instrument lookups. Requested live, peak, traffic and call counts exclude input data, normal application allocations, allocator headers, stack-owner size, and process memory. Index construction belongs to memory accounting. Pipeline counters cover the complete process, including all three detector repetitions. They do not give a per-detector retained-memory figure or a process RSS peak.

The micro uses double and `size_t` triples at 4,096 and 524,288 entries. Each sample reserves, inserts the fixed seed-417 shuffle, and performs three successful-hit passes. It has one warmup per backend and nine samples. Four orders rotate; custom runs first three times and each alternative twice. Construction, destruction, and complete stored-key verification are outside the timer. Reserve is timed. Cardinality, every retained key, and the ID checksum agree. All reported ranges include every sample.

Both models use two fresh processes per backend, three detector repetitions per process, and reversed backend order for the second pair. Import, source preparation before the timer, rendering and GPU work are excluded. The existing driver runs the stage batches returned by `nextComparisonStage`, including the combined automatic-detector batch. Every `computeComparisonStages` call receives empty `analysisInputs`. Results are destroyed after each stage body. The driver does not reuse the application analysis cache. First and later repetitions can differ through allocator, OS and input-data warmth.

The final three targets build without warnings or errors in Debug and Release. Both contracts pass in both configurations, 2/2 per configuration. The actual Release contract output reports 9,000 growth records compared, two signed-zero/finite contracts, and the capacity limits above. Growth, reserve, copy, assignment, move, reused moved-from owners, key order, IDs, and allocation/address stability on hits are checked for every tuple type and arity in the three modules. Capacity rejection checks the exception and unchanged cardinality. Its preflight precedes index allocator requests; exception construction can allocate through the untracked runtime allocator. See [build verification](benchmarks/simplification-19-dense-set/build-validation.json) and [actual contract output](benchmarks/simplification-19-dense-set/release-contract-detail.txt).

The initial inline global exit reporter did not emit in Release. That partial pilot stopped after the first custom Bus process. Its [CSV and exact provenance](benchmarks/simplification-19-dense-set-pilot/pilot-provenance.json) are excluded from final comparisons. The corrected function-local reporter is registered at the first tracked allocation. A separate [exit contract](../experiments/simplification-19-dense-set/exit-report-contract.ps1) runs both actual detector binaries on one temporary OBJ and checks positive allocation counters, one report per process, zero live bytes at exit, and equal complete count vectors. The unique fixture directory is removed on success and failure.

## Micro results

Times are median milliseconds for reserve, one insertion pass, and three hit passes over nine samples.

| Key family; entries | Current custom | Standard set, append/spare | Big-bucket set, append/spare | Standard set, find first |
| --- | ---: | ---: | ---: | ---: |
| Double triples; 4,096 | 0.5932 | 0.7919 | 0.8412 | 0.7507 |
| Double triples; 524,288 | 212.6350 | 238.2919 | 286.1875 | 205.5837 |
| Integer triples; 4,096 | 0.4554 | 0.7323 | 0.7859 | 0.6049 |
| Integer triples; 524,288 | 178.2080 | 182.1921 | 220.6929 | 174.6397 |

Standard append/spare regresses by 33.50% and 60.80% at the small coordinate/integer sizes, and by 12.07% and 2.24% at the large sizes. The big bucket regresses by 34.59% for large coordinates and 23.84% for large integers.

Find-first is faster than append/spare for every micro median. It hashes/looks up a miss twice and performs no temporary insertion on a hit. Against custom, its large medians improve by only 3.32% for coordinates and 2.00% for integers. Coordinate ranges overlap: 193.36–297.94 ms for find-first and 197.61–271.99 ms for custom. Integer ranges also overlap: 162.63–208.08 and 164.62–205.10 ms. Small find-first medians regress by 26.55% and 32.83%. These samples do not establish a reliable general lookup gain.

Both key families have 24-byte triples. Every repeated reserved sample has the same requested-memory ledger:

| Entries; route | Retained and peak requested bytes | Total requested bytes | Allocation calls |
| --- | ---: | ---: | ---: |
| 4,096; custom | 163,840 | 163,840 | 2 |
| 4,096; standard append/spare | 163,864 | 163,896 | 3 |
| 4,096; big append/spare | 196,632 | 196,680 | 3 |
| 4,096; standard find-first | 163,840 | 163,872 | 3 |
| 524,288; custom | 20,971,520 | 20,971,520 | 2 |
| 524,288; standard append/spare | 20,971,544 | 20,971,576 | 3 |
| 524,288; big append/spare | 25,165,848 | 25,165,896 | 3 |
| 524,288; standard find-first | 20,971,520 | 20,971,552 | 3 |

Custom and standard sets use 8,192/1,048,576 buckets at these sizes. Standard append adds one 24-byte key slot. Find-first needs no spare. Initial set buckets add 32 bytes of allocation traffic, or 48 bytes for the big bucket. The big bucket raises retained payload by 20% plus the spare. The set removes the duplicate key storage of the earlier map adapter, whose large retained payload was 36 MiB. It does not remove another retained-key copy from custom: custom already stores each key once.

## Current detector batches

Times are median milliseconds with full six-sample ranges. These instrumented custom/set results are paired within this campaign. Do not pool their times with the earlier uninstrumented map campaign.

| Model | Instrumented custom | Standard set, append/spare | Median change |
| --- | ---: | ---: | ---: |
| BusGameMap | 1492.15 (1409.24–1506.48) | 1601.62 (1548.52–1696.74) | +7.34% |
| Powerplant | 18215.65 (18137.10–18453.20) | 18752.45 (18579.10–18936.00) | +2.95% |

Every same-model/order/repetition set sample is slower than its custom sample: +4.36% to +18.06% for Bus and +1.68% to +4.40% for Powerplant. The full ranges do not overlap. There are only two fresh-process pairs per model; these ranges are observations, not confidence intervals or guarantees for other machines.

| Model | Custom first / later median ms | Set first / later median ms |
| --- | ---: | ---: |
| BusGameMap | 1503.42 / 1459.58 | 1644.00 / 1592.04 |
| Powerplant | 18295.15 / 18215.65 | 18856.40 / 18727.50 |

First medians use the first repetition of two processes. Later medians use four remaining repetitions. These are not application-cache cold/warm measurements.

Both three-run processes for each route give identical allocation counters:

| Model; route | Peak index requested bytes | Three-run requested traffic bytes | Allocation calls | Exit live bytes |
| --- | ---: | ---: | ---: | ---: |
| Bus; custom | 25,907,616 | 141,479,880 | 18 | 0 |
| Bus; set | 25,907,648 | 141,474,864 | 36 | 0 |
| Powerplant; custom | 484,733,792 | 2,287,715,448 | 18 | 0 |
| Powerplant; set | 484,733,824 | 2,086,383,840 | 36 | 0 |

The set has no useful aggregate peak reduction. Bus traffic changes by only -0.0035%. Powerplant traffic falls by 201,331,608 bytes, or 8.80%, over three repetitions. Calls double. Zero live at normal process exit confirms the recorded index owners were released.

The peak has a concrete capacity explanation. Triangle diagnostics reserve a double quadruple index for `R` render vertices. The measured peak equals `32R + 8B`, where `B` is the bucket count, plus a 32-byte spare on the set route. [Item 14's matching inputs](simplification-14-results.json) have `R=547,469` for Bus and `R=10,953,627` for Powerplant. Both routes choose the same diagnostic bucket count: `2^20` and `2^24`, respectively.

Powerplant's separate point-group reserve uses 5,984,083 source points. Custom's reserve formula chooses `2^24` buckets; the standard set's normal 0.8 load factor chooses `2^23`. This capacity calculation removes 64 MiB of bucket requests from that construction. The three-run traffic reduction is consistent with this repeated saving and small-index differences. That owner is below the larger diagnostic-index peak. It does not lower the recorded process-wide index maximum. Neither figure estimates full application RAM, graph payload, retained cache memory, or GPU memory.

All nine enabled diagnostic counts agree in all 24 final samples. The input headers are `triangles=1054542 source_points=535893 parts=65` and `triangles=12759246 source_points=5984083 parts=21`. In category order, the vectors are:

| Model | Boundary, non-manifold edges, winding, duplicate points, duplicate triangles, degenerate triangles, non-manifold vertices, holes, fins |
| --- | --- |
| Bus | 17058, 0, 0, 510, 0, 270, 4, 22, 21 |
| Powerplant | 119819, 1079616, 364, 559509, 58099, 60814, 67687, 19, 3261 |

The [preserved-campaign comparison](benchmarks/simplification-19-dense-set/preserved-count-comparison.json) also checks every new process against all six preserved processes of that model. It checks file hash/size/header, all run/stage layouts, empty vectors on source/total rows, and all count fields. All 48 input-identity comparisons, 432 stage-row comparisons, and 1,296 individual count-field comparisons pass. The production index and driver hashes match the earlier campaign. Count equality does not establish full finding, source-order, reference, status, or export equality.

## Find-first scope and merge decision

The micro contains 75% successful hits. The real source paths have a different mix. Duplicate-point grouping calls the index once per source point. The harness selects the whole OBJ, and OBJ has no `originalPointIds` deduplication layer. Its exact hit count equals the reported extra duplicate-point count: 510 of 535,893 calls for Bus, or 0.0952%; 559,509 of 5,984,083 for Powerplant, or 9.3500%. Thus this triple-index path is mostly insertion. Find-first adds a second lookup/hash to those misses.

Triangle diagnostics use `remap` to skip the index after the first use of a render ID. They do not perform one lookup for every triangle corner. Their geometric seam/duplicate hits are not measured separately. The timed harness uses default `originalIndex` topology, with one transform for all parts. Geometry construction therefore uses `originalIds` and bypasses its large coordinate/original-vertex hash. Transform indexing is small. Exact-position topology can have a different, hit-heavy corner workload, but it is not the measured default route.

The 2–3% large-micro find-first gains, overlapping ranges, small-case regressions, and insertion-heavy real paths do not establish a decision-changing gap for the current general index. The completed append/spare pipeline already regresses on both full models and has no index-peak benefit. Keep custom as the default. Find-first remains an unmeasured pipeline alternative; this report does not claim that every library adapter must regress or that a specific hit-heavy workload cannot benefit. No additional campaign is required to reject the tested general substitution.

Implementation is feasible within the checked domain, but adoption must first resolve the wide fallback, all header consumers, complete findings and source order, both topology modes, mixed transforms, shared analysis-cache reuse, cancellation, bounded-result status, and export equivalence. Allocation failure during spare maintenance also needs an explicit exception-state contract. A worthwhile future candidate must show useful full-workload gains with normal production allocation and measure application peak/retained memory if memory is its reason for adoption. The current data provide no basis to merge a general container replacement to main.
