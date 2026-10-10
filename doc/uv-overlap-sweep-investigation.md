# Incremental UV overlap sweep

The sweep has since been adopted in production with the new deterministic partial-result order. See the [implementation report](uv-overlap-sweep-implementation.md). The measurements and recommendations below describe the preceding prototype investigation.

## Outcome

The incremental sweep is a better production candidate than the eager or per-patch box trees. It completes disjoint strips without the current scan's candidate-limit failure, while avoiding most of the trees' setup cost. On BusGameMap, bounded analysis takes about 2% more time within patches and 9.5% more across selected patches than the current scan. It is not a general speed improvement.

The main behavior decision is partial-result ordering. Complete small results match the production scan, but a capped sweep can highlight a different subset of triangles. Across selected patches on BusGameMap, the current scan marks 10,001 triangles; the sweep marks 308. Both retain 10,000 pairs and report a partial result. This must be an explicit behavior change if the sweep is adopted.

The [prototype](../experiments/uv-overlap/prototype.cpp) remains in the opt-in experiment. No application source or saved settings changed. The production baseline is commit `4bfb927`; the earlier [box-tree report](uv-overlap-broad-phase-investigation.md) provides the previous investigation.

## Algorithm and ordering

The sweep first creates valid UV boxes and sorts them by patch, minimum U, and source triangle index, as the current scan does. Selected-patch scope omits the patch sort key. It then processes one box at a time.

An AVL tree stores active V intervals. Each node contains its interval, child indices, height, and the largest maximum V in its subtree. A min-heap identifies intervals whose maximum U is at or before the current minimum U. These intervals are removed before the query, so contact at the U boundary is excluded. V comparisons are also strict. A subtree whose maximum V is at or below the current minimum V is rejected without scanning its intervals.

The query checks active intervals in ascending minimum V, with sweep rank as the tie-breaker. It runs the existing exact triangle predicate immediately for each box candidate and stops at the candidate or pair limit. The new box enters the tree only after its query completes. Expired node slots are reused. Per-patch scope clears active state at each patch boundary.

The old pair order is earlier U rank first, then later U rank. The sweep order is later U rank first, then the earlier interval's minimum V and U rank. Each pair still stores the earlier U triangle in `first`. Sorting the final retained pairs would not restore the old capped subset: the unvisited pairs are absent. Tests therefore compare complete sets with the current scan and compare capped prefixes with an independently constructed sweep order.

## Bounded work and cancellation

The prototype retains the default limits of two million exact-test candidates and ten thousand retained pairs. A candidate means boxes overlap in both dimensions. This differs from the production counter, which includes boxes disjoint in V.

Queries and AVL updates also consume a separate experimental search allowance of `64 * total triangles + 4 * candidate limit`. Exhausting it publishes the completed pair prefix as partial. The probe records search exhaustion separately. This allowance is not a calibrated production constant. Initial box creation and sorting, heap comparisons, and constant-size rotations are outside the counter; insertions and removals still have logarithmic tree and heap cost. A work counter is not a wall-clock deadline.

Cancellation is checked during box creation, each event, and recursive queries and updates. A cancellation exception prevents publication of the new result. The initial standard-library sort has no internal stop check, as in the current implementation. Production adoption should add deterministic tests for cancellation during work; the probe currently tests cancellation before work.

## Performance

Measurements use four Release samples on an AMD Ryzen 7 5800H with the `vs2026-vcpkg` preset. All four algorithms run in the same process and rotate order so each runs first once. The table reports the median, computed as the mean of the two middle samples. No build or test runs during measurements.

UV measurement and file loading occur before timing. Timings include overlap storage, sorting/indexing, exact predicates, and statistics updates. They exclude the copy of the presentation container, display preparation, GPU work, and worker scheduling. Angle statistics are already cached. The [raw results](uv-overlap-sweep-results.json) contain samples, counters, source and executable hashes, and the model hash.

| Workload | Current scan | Sweep | Result |
| --- | ---: | ---: | --- |
| 2,001 shuffled disjoint rows | 14.69 ms | 1.60 ms | Scan partial; sweep complete |
| 2,001 shuffled disjoint columns | 0.51 ms | 0.56 ms | Both complete |
| 10,000 shuffled disjoint rows | 21.53 ms | 10.10 ms | Scan partial; sweep complete |
| 10,000 shuffled disjoint columns | 2.86 ms | 3.08 ms | Both complete |
| 10,000 disjoint grid triangles | 5.09 ms | 6.04 ms | Both complete |
| 1,000 coincident triangles | 453.15 ms | 449.56 ms | Both stop at 10,000 pairs |
| 512 disjoint thin diagonal triangles | 8.20 ms | 9.81 ms | Both complete; 130,816 candidates |
| BusGameMap, within each patch | 1,096.79 ms | 1,118.57 ms | Both partial |
| BusGameMap, selected patches together | 290.05 ms | 317.55 ms | Both partial |

BusGameMap contains 1,054,542 triangles, including 958,674 valid UV triangles. Within patches, the eager tree takes 2,055.40 ms and the lazy tree 1,942.66 ms in this run. Across selected patches, they take 1,515.59 ms and 1,522.94 ms. The sweep avoids those large regressions because it does not index the full selection before its first exact test.

| Model scope | Current candidates / pairs | Sweep candidates / pairs | Current / sweep affected triangles |
| --- | ---: | ---: | ---: |
| Within patches | 2,000,000 / 9,566 | 21,051 / 10,000 | 2,800 / 2,100 |
| Selected patches together | 17,365 / 10,000 | 13,274 / 10,000 | 10,001 / 308 |

Across selected patches, all 10,000 retained current-scan pairs are cross-patch; 9,318 retained sweep pairs are cross-patch. The rest are within a patch. The changed order concentrates the early sweep result in a smaller part of the selection. These timings compare bounded operations, not equal completed work or identical retained results. No full million-triangle pair-set comparison is claimed.

The sweep's temporary vector capacity is about 40.2 MiB within patches and 40.1 MiB across selected patches. Peak active interval counts are 2,100 and 261. The box trees use about 66.4 MiB and 74.4 MiB, respectively. These are total scratch capacities, not process peak memory. They exclude measurements, result flags and pairs, and allocator overhead. The sweep still creates a box for every valid triangle, so total scratch memory remains proportional to input size even when the active index is small.

## Recommendation for production

Use the sweep as the leading candidate if a new deterministic partial-result order is acceptable. Its main benefit is completeness on sparse arrangements that overlap along U, with much less setup cost than the tested box trees. Retain the current exact predicate, strict boundary rules, scope separation, immutable result ownership, and worker publication checks.

Before promotion, specify that capped highlights are a partial subset rather than coverage of the whole selection. Record whether search work, exact candidates, or retained pairs caused truncation. Set and validate the search allowance separately from the exact-candidate limit, and handle size arithmetic without overflow.

If preserving the existing capped subset is required, this sweep is not a drop-in replacement. Keep the production scan until an alternative with that ordering contract is available. The test evidence does not support hiding the difference by sorting a truncated list.

A small direct-scan path could reduce overhead for short active sets, but it must enumerate candidates in the same V order to preserve the new contract. That optimization was not measured here. Dense overlap and long diagonal boxes remain expensive under any axis-aligned broad phase, so limits and partial-result reporting remain necessary.

## Validation

The expanded contract checks complete pair sets, flags, counts, and findings against the production scan for 30 seeded random fixtures in both scopes, plus a 300-triangle fixture with mixed interval lifetimes. It covers mirrored and invalid triangles, containment, boundary contact, extreme UV scales, empty inputs, both strip orientations, disabled analysis, and cancellation before work.

An independent ordering oracle checks capped sweep prefixes and repeatability. Exact candidate and pair limit boundaries are tested, including zero and exactly complete limits. Search limits of 0, 1, 10, and 100 check that work does not exceed its allowance. During contract tests, every successful insertion and removal validates AVL balance, height, maximum-V augmentation, key ordering, and active count. This validation is disabled in timed samples.

Build and run with `WOBY_BUILD_BENCHMARKS=ON`:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_uv_overlap_probe
& build/vs2026-vcpkg/bin/Release/woby_uv_overlap_probe.exe --self-test
& build/vs2026-vcpkg/bin/Release/woby_uv_overlap_probe.exe --synthetic 4
& build/vs2026-vcpkg/bin/Release/woby_uv_overlap_probe.exe 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj' 4
```

The Release probe and full Debug project compile without warnings. All 80 unit groups pass, including the expanded `woby_uv_overlap_contract`; the full run took 49.64 seconds. `git diff --check` passes. Validation logs are under `build/uv-sweep-*.log`.
