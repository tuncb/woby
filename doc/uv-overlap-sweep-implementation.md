# Production UV overlap sweep

## Changes

[`inspectUvOverlaps`](../src/uv_overlap.cpp) now uses the incremental U sweep and active V-interval index. The exact triangle predicate is unchanged. Sparse triangles that overlap only along U no longer consume the exact-candidate allowance. The 2,001-row regression fixture completes with zero candidates in either axis orientation.

The active index is an AVL tree with subtree maximum V. A heap removes expired U intervals before each query. Removed node slots are reused, and per-patch scope clears the active index between patches. The implementation uses structs and free functions, runs in the existing worker path, and retains immutable overlap ownership and cache matching.

The accepted ordering is named `uv_sweep_v1` in the control output:

1. Process valid boxes by patch when scoped per patch, then minimum U and source triangle index.
2. Query earlier active boxes by minimum V, then their U sweep rank.
3. Store the earlier box as `first` and the current box as `second`.

This changes both pair order and the retained subset when a limit stops the search. Complete overlap sets remain equivalent. On BusGameMap, selected-patch scope retains 10,000 pairs affecting 308 triangles, including 9,318 cross-patch pairs. The previous scan retained 10,000 cross-patch pairs affecting 10,001 triangles. Both results are partial. Sorting the retained list would not restore the previous subset.

## Limits and diagnostics

The default exact-candidate and retained-pair limits remain 2,000,000 and 10,000. `candidateCount` now counts only boxes that overlap strictly in both dimensions.

Queries and AVL updates have a separate search allowance. The default is `64 * valid triangle count + 4 * candidate limit`, with saturating arithmetic. An optional internal `UvOverlapLimits::searchWork` overrides it, including zero. The limit participates in overlap cache equality. It is not a new scene control or saved property.

`UvOverlapResult` records search work, its effective limit, and a truncation reason. Control output adds `searchWorkCount`, `searchWorkLimit`, `truncationReason`, and `pairOrder`. Reasons are `none`, `candidates`, `pairs`, and `search_work`. The existing `truncated` field remains. The UI and exported report identify the limiting resource and state that highlights cover retained pairs only. See the [control reference](ctl-commands.md).

Search accounting bounds recursive queries and updates; it is not a wall-clock deadline. Initial box construction and sorting remain input-sized work. Heap comparisons and constant-size rotations are not separate charges. Cancellation checks cover box creation, periodic sort comparisons, sweep events, and recursive index operations. An aborted index update is discarded with the temporary index; retained overlap pairs remain a completed deterministic prefix when a work limit is reached.

## Production measurements

The [probe](../experiments/uv-overlap/main.cpp) now compares the production sweep, the two box trees, the prototype sweep, and a [frozen scan from 4bfb927](../experiments/uv-overlap/scan_reference.inc). A source comparison verified that the frozen scan matches that commit and that the production exact predicate is unchanged. The old scan is compiled against the current headers and statistics code for this comparison.

These are medians of five Release samples on an AMD Ryzen 7 5800H with `vs2026-vcpkg`. Algorithm order rotates so each runs first once. No build or test runs during measurement. Timings include overlap inspection and statistics on already measured UV data; they exclude loading, UV measurement, presentation copies, display preparation, GPU work, and worker scheduling. [Raw results](uv-overlap-sweep-implementation-results.json) include all samples and source, executable, and model hashes.

| Workload | Frozen scan | Production sweep | Result |
| --- | ---: | ---: | --- |
| 2,001 disjoint rows | 14.67 ms | 1.71 ms | Scan partial; sweep complete |
| 2,001 disjoint columns | 0.56 ms | 0.59 ms | Both complete |
| 10,000 disjoint rows | 20.65 ms | 10.74 ms | Scan partial; sweep complete |
| 10,000 disjoint columns | 2.87 ms | 3.32 ms | Both complete |
| 10,000 disjoint grid triangles | 5.29 ms | 6.34 ms | Both complete |
| 1,000 coincident triangles | 427.01 ms | 424.39 ms | Both pair-limited |
| 512 disjoint thin diagonal triangles | 8.24 ms | 9.73 ms | Both complete |
| BusGameMap within patches | 1,192.22 ms | 1,304.01 ms | Both partial |
| BusGameMap selected patches | 355.82 ms | 295.37 ms | Both pair-limited |

The model samples varied widely. Within patches, the scan ranged from 1,052 to 2,398 ms and production from 1,117 to 2,104 ms. Across patches, the ranges were 314 to 557 ms and 293 to 494 ms. These model medians are not stable evidence of a general speed gain or a precise regression. The earlier [prototype run](uv-overlap-sweep-investigation.md) also showed that the sweep is not a general speed improvement.

BusGameMap has 1,054,542 triangles, including 958,674 valid UV triangles. Production uses 21,051 exact candidates and 62,865 search steps within patches, and 13,274 candidates and 22,345 search steps across selected patches. Both reach the pair limit well before the 69,355,136-step search allowance. Dense overlap and overlapping boxes around disjoint diagonal triangles still require exact work. The fixed limits and partial-result reporting remain necessary.

## Validation

- Full Debug project build and Release probe build: passed without warnings, using `vs2026-vcpkg`. The Debug build used `/p:LinkIncremental=false`.
- All 80 unit groups: passed in 51.74 seconds, including slow and graphics unit groups.
- `woby_uv_grid_layout_quality_render` and `woby_uv_quality_render`: passed in 83.70 seconds. The latter also checks the new control diagnostics, overlap rendering, picking, and scope changes.
- `git diff --check`: passed.

New production tests cover both strip orientations, independent all-pairs geometry and event-order comparison, invalid-triangle exclusion, patch scope, capped flags, exact limit boundaries, search-budget exhaustion, deterministic repeated prefixes, cached partial results, and saturated automatic-budget arithmetic. Existing tests cover contact classification, containment, mirrored UVs, cancellation, and stale worker rejection. The probe additionally compares production with the frozen scan and the prototype, including the prototype's AVL invariant checks. No new test hook was added to production for forcing cancellation at a specific internal phase.

The opt-in benchmark and investigation reports are included with the implementation. Reproduce the final comparison with `woby_uv_overlap_probe --synthetic 5` and `woby_uv_overlap_probe MODEL.obj 5` after building the Release target. Logs are under `build/uv-sweep-production-*.log`.
