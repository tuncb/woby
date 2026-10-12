# Shared annotation geometry

This change implements the shared-geometry step of [item 11](simplification-11-scene-investigation.md). The baseline is commit `9db20ba6ac34ad11679fe037e4bce148866fe76f`. The measurement date is 11 October 2026.

## Production change

`UiAnnotation`, `SceneAnnotationRecord` and completed annotation commands now hold an `AnnotationGeometryOwner`. The owner stores a `shared_ptr<const AnnotationGeometry>`. It includes the outline, sources, projectors, fingerprints and drawing controls. It has no mesh, GPU object or runtime cache.

Creation, reshape, file parsing and legacy coordinate migration build a separate geometry value. Operations and load boundaries validate that value before it enters the scene. Publishing moves its arrays into a const owner. A worker can pass its completed owner to an operation without another outline copy. A normal `.woby` load shares the parsed owner with live state; legacy migration publishes a new owner and leaves the parsed value intact.

Both representations in each history snapshot share the live owner. The clean document and temporary save records also share it. Duplicating an annotation shares its initial outline; a subsequent reshape replaces only the edited object's owner. Retaining a historical outline does not retain its source mesh.

Owner equality first checks pointer identity, then compares all geometry values when the owners differ. Thus, an independently loaded equal outline remains equal, a notified no-op does not create an undo action, and a coalesced drag that returns to its initial value still removes its action. No hash or revision replaces value equality.

Attachment validity, pending status and geometry revision stay outside the immutable geometry. History capture clears these session values. Restore validates attachments against current sources and advances the live geometry revision when the outline changes. Existing scene revision and worker identity checks reject late results. Derived world lines and projected lines stay in the query runtime.

The `.woby` fields and version remain unchanged. The parser uses mutable drafts, then publishes their validated values. `loadSceneAnnotations` prepares the complete annotation batch before replacing live annotations, so malformed later records cannot leave a partly loaded batch. Edit projections copy the definition without first copying and discarding the old segment array.

This implements the ownership gate and the applicable equality, runtime, persistence and integration gates. The separate typed group/file settings and single-content snapshot migration remain a later item 11 change. This patch retains the existing history structure and its navigation transactions.

## Production-path measurements

The [benchmark](../experiments/simplification-11/production.cpp) calls the actual production sequence: `setMasterVertexPointSize`, `recordSceneHistory`, then `updateSceneDirty`. Each measured sample contains 12 edits. Discrete edits retain 13 snapshots. A drag uses one interaction token and retains two snapshots. Each sample also checks undo/redo content equality outside its timer.

The same benchmark source was compiled before and after the change. The baseline executable was retained before modifying production code. The [paired runner](../experiments/simplification-11/run-production.ps1) uses five fresh processes per variant and workload, reversing variant order on alternate runs. It runs sequentially with no concurrent build or test. [Raw samples and executable hashes](benchmarks/shared-annotation-geometry/paired/results.json) and [the derived summary](benchmarks/shared-annotation-geometry/paired/summary.json) are saved.

The host is Windows 11 with a Ryzen 7 5800H. Both executables use Release, MSVC 19.51.36243.0 and the `vs2026-vcpkg` preset. Both use the same requested-allocation recorder. Its header and atomic counters affect absolute time. These are CPU times for the production operation sequence, not whole-frame times. Initial scene construction, geometry publication, the clean document, initial history capture, rendering and file IO are outside the timer. All outlines in this campaign are synthetic valid repeated triangle anchors; the 147-segment case is not a real-scene import.

Median milliseconds for all 12 edits are below. Brackets show the full five-sample range.

| Segments; groups | Action | Before | Shared owner |
| --- | --- | ---: | ---: |
| 147; 1 | Discrete | 0.3312 [0.3102, 0.4262] | 0.0785 [0.0650, 0.0842] |
| 147; 1 | Drag | 0.1317 [0.1208, 0.2068] | 0.0628 [0.0610, 0.0694] |
| 20,000; 1 | Discrete | 13.4190 [12.7160, 15.9898] | 0.0836 [0.0711, 0.1625] |
| 20,000; 1 | Drag | 17.1014 [16.5246, 19.9683] | 0.0726 [0.0629, 0.1381] |
| 1,000,000; 1 | Discrete | 706.4522 [638.1558, 761.9058] | 0.0963 [0.0883, 0.1451] |
| 1,000,000; 1 | Drag | 791.3877 [735.4089, 1,124.3455] | 0.0801 [0.0787, 0.1604] |
| 0; 256 | Discrete | 3.4576 [2.6197, 4.2758] | 3.0148 [2.7288, 4.0892] |
| 0; 256 | Drag | 4.9120 [4.1144, 5.7539] | 5.9790 [4.7092, 6.8140] |
| 0; 16,384 | Discrete | 179.2236 [163.6483, 268.2852] | 176.6949 [158.0980, 260.0400] |
| 0; 16,384 | Drag | 184.5066 [173.4687, 196.4828] | 255.0432 [176.2525, 313.9392] |

The large-outline gain removes geometry copies and equality scans for unchanged owners. It does not predict reshape cost: distinct historical outlines still require distinct arrays. The controls without annotations have unchanged allocation counts and payloads, apart from the temporary path's small string-size variation. No general metadata speedup is claimed.

The first control medians suggested slower drags without annotations. A [follow-up](benchmarks/shared-annotation-geometry/control/results.json) used two processes per variant, nine samples in each process and reversed process order. At 16,384 groups, its medians were 144.025 / 165.026 ms for discrete edits and 152.343 / 174.207 ms for drags: 14.6% and 14.4% higher for the candidate, with wide overlapping ranges.

A [second follow-up](benchmarks/shared-annotation-geometry/control-affinity/results.json) used the same two-process, nine-sample sequence with both variants assigned to CPU affinity mask `4` and process priority `AboveNormal`. Only the benchmark child processes received these settings. Its 18-sample medians and ranges are below.

| Groups; action | Before | Shared owner |
| --- | ---: | ---: |
| 256; discrete | 2.5227 [2.3206, 4.1171] | 2.4500 [2.2471, 2.7674] |
| 256; drag | 3.7282 [2.9703, 5.7039] | 3.5154 [2.9646, 4.2226] |
| 16,384; discrete | 151.2932 [119.3735, 219.4939] | 154.1991 [120.5965, 215.6268] |
| 16,384; drag | 169.6710 [133.2984, 232.8762] | 160.7732 [129.2346, 247.8656] |

The large control changes to +1.9% for discrete edits and -5.2% for drags. The slowdown is not stable across execution conditions. These controls do not establish a metadata benefit or a fixed regression; the exact cause of the timing variation is not isolated. All campaigns remain in the evidence. The adoption decision rests on the unchanged metadata workload and allocation counts, the ownership tests, and the repeatable removal of large outline copies.

## Memory

The payload calculation deduplicates geometry owners across live state, the clean document and both representations in every retained history snapshot. It counts segment vector capacity, at 48 bytes per segment. It excludes owner/control-block metadata, allocator overhead, process RSS and GPU memory.

| Outline | Discrete: before / after | Drag: before / after |
| --- | ---: | ---: |
| 147 segments | 197,568 / 7,056 B | 42,336 / 7,056 B |
| 20,000 segments | 26,880,000 / 960,000 B | 5,760,000 / 960,000 B |
| 1,000,000 segments | 1,344,000,000 / 48,000,000 B | 288,000,000 / 48,000,000 B |

The discrete case changes from 28 geometry values to one: a **96.4%** segment-payload reduction. At one million segments, this is **1,281.7 to 45.8 MiB**. The drag changes from six values to one, an **83.3%** reduction. These totals include live and clean owners, which the original prototype's history-only comparison excluded.

Requested C++ allocations during the 12 one-million-segment edits are below. These extra retained and peak bytes exclude owners already present before the timer. The 48 MB shared outline already exists at that point; it must not be counted as zero total scene memory.

| Action | Extra retained: before / after | Extra peak: before / after | Cumulative allocation traffic: before / after |
| --- | ---: | ---: | ---: |
| Discrete | 1,152,048,328 / 41,632 B | 1,200,049,559 / 42,584 B | 1,728,075,740 / 66,080 B |
| Drag | 96,004,022 / 3,464 B | 192,006,788 / 5,672 B | 1,728,061,796 / 52,136 B |

## Integration checks

The new and extended tests cover shared owners in live/clean/history records, independent equal owners, no-op edits, return-to-start drags, independent duplicate reshapes, source-mesh release, save/load field equality, malformed load batches, source removal and reload, missing and changed inputs, failed multi-file reload, late worker results, and monotonic geometry revisions through undo/redo. Existing view, coordinate-origin, annotation, query, render and cancellation tests cover the other consumers.

Save/load fixtures use one unique temporary directory per test. Related source and scene paths are absolute or resolved against an explicit scene path in that directory. They do not depend on the build directory's drive.

The final `vs2026-vcpkg` Debug build passed with zero warnings. All **129 CTest entries passed**, with no failed, disabled or skipped entries, in **530.14 seconds**. This includes the CPU, graphics and slow integration suites. The Release benchmark/probe build had no warnings; both affected Release probe contracts passed. The [validation record](benchmarks/shared-annotation-geometry/validation.json) preserves the result. The historical test count includes retired experimental targets. The raw Debug build and test logs were removed with those targets.

## Reproduction

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_SIMPLIFICATION_PROBES=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j2
cmake --build --preset vs2026-vcpkg --config Release --target woby_annotation_history_benchmark
./experiments/simplification-11/run-production.ps1 -BaselineBinary <baseline.exe> -OutputDirectory <new-directory>
```

For the controls, add `-MetadataOnly -Samples 2 -SamplesPerProcess 9`. Add `-ProcessorAffinity 4` to repeat the affinity run.

To rebuild the baseline, use commit `9db20ba6ac34ad11679fe037e4bce148866fe76f` in a separate Windows worktree under `D:/.worktree`. Copy this benchmark's `production.cpp` and its three CMake target lines into that checkout's existing `experiments/simplification-11` directory. Build with the same Release preset and retain the executable. The generic geometry accessor lets the identical benchmark source compile against both representations. The [captured benchmark source](benchmarks/shared-annotation-geometry/benchmark-source.cpp) records the source used for this comparison. Run variants serially and retain a new output directory for each campaign.
