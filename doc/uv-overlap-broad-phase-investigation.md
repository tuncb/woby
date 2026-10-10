# Two dimensional UV overlap broad phase

The recommended incremental sweep has since been prototyped. See the [follow-up report](uv-overlap-sweep-investigation.md) for its results and the change to bounded pair ordering. The measurements below preserve the earlier box-tree comparison.

The sweep is now used in production; see the [implementation report](uv-overlap-sweep-implementation.md). The probe retains the old scan in `scan_reference.inc` for independent comparisons.

## Recommendation

Keep the current production scan while testing an incremental sweep with an interval index on V. A two-dimensional search fixes a real completeness problem, but the box-tree prototypes tested here are unsuitable as a general replacement. Their index construction costs more than the work they remove on BusGameMap, especially when the pair limit stops the search early.

This investigation uses production commit `4bfb927`. Application behavior is unchanged. The [opt-in probe](../experiments/uv-overlap/main.cpp) and [tree prototype](../experiments/uv-overlap/prototype.cpp) compare two alternatives against the linked production function. The prototype includes the production translation unit under a renamed entry point so it can use the exact same narrow-phase predicate. It does not duplicate or change the intersection rule.

## Current behavior

[`inspectUvOverlaps`](../src/uv_overlap.cpp) sorts valid triangle boxes by patch, then minimum U and triangle index. For each box, it scans later boxes until their U ranges separate. It counts each scanned pair before checking V. The defaults are two million candidates and ten thousand retained pairs.

For 2,001 disjoint horizontal rows, the full scan needs 2,001,000 U candidates. It reaches the candidate limit with no overlap findings and marks the result partial. Exchanging U and V produces no candidates and a complete result. This is an orientation-dependent completeness limit; it is not an incorrect triangle intersection.

The expensive path runs when overlap is enabled, source geometry changes, or overlap scope changes. Existing result caching already avoids this work for threshold, normalization, metric, and surface/layout changes when the measurements and overlap scope remain valid. This proposal therefore targets overlap calculation, not frame rendering or ordinary picking.

## Prototype design

Both prototypes build a balanced box tree with up to eight boxes per leaf. They split on the axis with the larger span of box centers. A query rejects a whole node when its box is disjoint on either axis. Patch scope uses separate roots; selected-patch scope uses one root.

The eager version builds all roots before the first query. The lazy version builds a root only when the search reaches its patch and reuses node storage for the next patch. Both still create and sort the full box list first.

Queries return only boxes that overlap strictly in both dimensions. Hits are sorted back into the original scan order before the exact predicate runs. This retains the existing ordered-pair prefix when the pair limit is the stopping condition. Candidate-limited results can extend further because the prototype counts only two-dimensional box candidates.

Tree navigation has a separate experimental work limit: `64 * total triangles + 4 * candidate limit`. Node and leaf box tests consume this limit. If it is reached, the current unfinished query is discarded and the already completed prefix is published as partial. This allowance is a probe parameter, not a recommended final production constant. Index construction and sorting are outside that counter; their cost is proportional to input size and tree construction work. Cancellation checks remain in construction, queries, and exact testing. Standard-library sort and partition calls do not have internal stop checks.

## Measurements

Results are medians of three Release samples on an AMD Ryzen 7 5800H, using `vs2026-vcpkg`. Algorithm order rotates between samples. Loading and UV measurement happen before timing. Timings include overlap allocation, indexing, exact tests, and statistics updates; they exclude quality copies, display preparation, GPU work, and worker scheduling. Angle statistics are already available. No build or test runs during timing. The [raw results](uv-overlap-broad-phase-results.json) include individual samples, counters, and model, source, and executable hashes.

| Workload | Current scan | Eager tree | Lazy tree | Result |
| --- | ---: | ---: | ---: | --- |
| 2,001 shuffled disjoint rows | 15.02 ms | 1.98 ms | 2.06 ms | Scan partial; trees complete |
| 2,001 shuffled disjoint columns | 0.52 ms | 1.48 ms | 1.40 ms | All complete |
| 10,000 shuffled disjoint rows | 21.33 ms | 13.04 ms | 12.89 ms | Scan partial; trees complete |
| 10,000 shuffled disjoint columns | 2.85 ms | 9.20 ms | 8.89 ms | All complete |
| 10,000 disjoint grid triangles | 5.10 ms | 13.48 ms | 13.39 ms | All complete |
| 1,000 coincident triangles | 440.01 ms | 431.86 ms | 428.11 ms | All stop at 10,000 pairs |
| 512 disjoint thin diagonal triangles | 8.25 ms | 10.18 ms | 10.26 ms | All complete; same 130,816 candidates |
| BusGameMap, within each patch | 1,113.55 ms | 2,036.17 ms | 1,952.67 ms | All partial; different stopping limits |
| BusGameMap, selected patches together | 294.44 ms | 1,516.89 ms | 1,531.47 ms | All stop at 10,000 pairs |

The model is `D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj`: 1,054,542 triangles, including 958,674 valid UV triangles. The 7.1 GB BearTrap model was not needed for this comparison. The previous user-selected fallback model already exposes a large regression.

Within patches, the current scan stops at two million candidates and 9,566 pairs. Both trees reach 10,000 pairs after 20,446 candidates. All three affect 2,800 triangles at their stopping points. Thus the extra pairs do not add highlighted triangles in this bounded result, and the trees still do not complete the model. This timing compares bounded user-visible operations, not equal amounts of completed analysis.

Across selected patches, all three reach 10,000 cross-patch pairs after 17,365 candidates and affect 10,001 triangles. The lazy tree spends about 1,385 ms building its index, out of 1,531 ms total. Here the current scan stops early enough that almost all tree construction is unnecessary.

The eager tree uses about 66.4 MiB of temporary vector capacity within patches and 74.4 MiB across selected patches. The lazy version has the same measured capacities on this model. These totals include boxes, index order, nodes, query hits, and roots; they exclude measurements, result flags and pairs, allocator overhead, and process peak memory. They are total prototype scratch storage, not an increase measured against the old scan.

## Benefits and costs

Two-dimensional pruning removes candidates that can be rejected from their UV boxes alone. It makes disjoint horizontal strips complete within the tested budget and avoids their quadratic pair scan. The exact predicate still decides containment, coincidence, and boundary contact. Patch scope, source indices, and report ordering can remain intact.

Index construction, indirect reads, and sorting query hits add cost. A simple scan is better when U already separates most boxes or a dense selection reaches the pair limit quickly. Building per patch saves little when the search starts in a large patch. A persistent tree would add retained memory, while the existing result cache already handles many repeat operations without another overlap search.

A box index cannot remove all quadratic work. Thin diagonal triangles can have heavily overlapping boxes without overlapping interiors. Truly dense overlap also has quadratic output. Candidate limits, pair limits, cancellation, and clear partial-result reporting must remain.

Changing candidate meaning needs an explicit contract. Two million one-axis scans and two million two-dimensional candidates represent different amounts of work. The production change should distinguish search work from exact-test candidates and record which limit stopped the analysis. A limit on exact tests alone does not bound a bad tree traversal.

## Recommended next steps

1. Prototype an incremental sweep in U with an active interval index on V. Remove intervals whose maximum U equals or precedes the next minimum U, so boundary contact does not become a box candidate. Build the active structure as the sweep advances, allowing dense inputs to stop before indexing the full selection.
2. Define deterministic event and candidate order. A sweep naturally changes the order in which pairs are found. Either specify that new order for bounded reports or measure the cost of preserving the current pair prefix. Sorting a truncated final list alone cannot recover the old prefix.
3. Compare a small direct-scan path with the indexed path on these same workloads. Set any crossover from measured work, rather than only triangle count. Keep the exact predicate unchanged.
4. Promote an implementation only after complete small results match a reference, sparse strips finish in both orientations, dense work stays bounded, and the model timings avoid the large regressions shown here. Add production tests for search-budget exhaustion, stable capped results, and cancellation during index construction and traversal.

## Validation and reproduction

The probe contract checks ordered pair equality with the production scan on 30 seeded random fixtures in both scopes, pair-limit prefixes, mirrored triangles, containment, shared edges and vertices, invalid UV exclusion, and UV scales of `1e-100`, `1`, and `1e100`. It also checks empty inputs, dense candidate and pair limits, repeatability, disabled overlap, cancellation before work, explicit zero search budget, and complete disjoint strips after swapping axes. The exact predicate is shared; these are broad-phase equivalence checks, not an independent proof of the predicate.

Build the optional target with `WOBY_BUILD_BENCHMARKS=ON` in the `vs2026-vcpkg` preset. Then run:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_uv_overlap_probe
& build/vs2026-vcpkg/bin/Release/woby_uv_overlap_probe.exe --synthetic 3
& build/vs2026-vcpkg/bin/Release/woby_uv_overlap_probe.exe 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj' 3
```

The full Debug project and Release probe build without compiler warnings. All 80 unit groups pass: 66 through the normal preset and the remaining 14 slow/graphics unit groups in a separate run. The probe is registered as `woby_uv_overlap_contract`. `git diff --check` passes. Validation logs are under `build/uv-overlap-*.log`.
