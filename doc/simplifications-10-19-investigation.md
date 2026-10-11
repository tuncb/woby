# Investigation of simplification items 10–19

This report covers the remaining proposals in [the original assessment](data-structure-algorithm-simplifications.md), with isolated correctness and performance experiments against commit `46a27585c5bc8638a8bde5d601e5cead71376631`. Investigation date: 11 October 2026. Production optimization code has not been changed.

## Numbering and coverage

The source table numbers proposals through 12. Its final section has six unnumbered paragraphs. This report uses the following working numbers, and covers every topic in those paragraphs. Item 19 includes the hash-index and already-removed parser-copy clauses from the last paragraph.

| Item | Topic | Detailed report | Raw measurements |
| --- | --- | --- | --- |
| 10 | Predicate-only exact intersections | [Predicates](simplification-10-predicates-investigation.md) | [Results](simplification-10-predicates-results.json) |
| 11 | Persistent scene content and history | [Scene/history](simplification-11-scene-investigation.md) | [Results](simplification-11-results.json) |
| 12 | Typed graphics parameters | [Graphics](simplification-12-graphics-investigation.md) | [Results](simplification-12-results.json) |
| 13 | Source-local distance queries and instances | [Instance queries](simplification-13-instances-investigation.md) | [Results](simplification-13-results.json) |
| 14 | Direct OBJ corner indexing | [OBJ corners](simplification-14-obj-investigation.md) | [Packing](simplification-14-results.json), [capacity trim](simplification-14-capacity-results.json) |
| 15 | GPU annotation projection | [Annotations](simplification-15-annotations-investigation.md) | [Results](simplification-15-results.json) |
| 16 | Compact topology slices and IDs | [Topology](simplification-16-topology-investigation.md) | [Initial](simplification-16-checked-results.json), [proved casts](simplification-16-proved-results.json), [envelope](simplification-16-envelope-results.json) |
| 17 | Shared spatial infrastructure | [Spatial audit](simplification-17-spatial-investigation.md) | Node sizes in item 13 |
| 18 | Broad geometry-library replacement | [Library audit](simplification-18-library-investigation.md) | Input-contract counts in item 16 |
| 19 | Analysis hash replacement and obsolete parser copy | [Hash/parser audit](simplification-19-hash-investigation.md), [dense set](simplification-19-dense-set-investigation.md) | [Map adapters](simplification-19-results.json), [dense set](simplification-19-dense-set-results.json) |

## Method and interpretation

Experiments use Windows 11, an AMD Ryzen 7 5800H with eight cores and 16 threads, and 68,564,348,928 bytes of reported physical memory. They use the `vs2026-vcpkg` preset, MSVC compiler 19.51.36243.0 / toolset 14.51.36231, CMake 4.3.2 and Release optimization. No benchmark runs alongside a build, test or another benchmark. CPU thermal state and OS scheduling are not controlled. Compare medians and ranges within one probe; the data do not constitute a confidence interval or an application frame-time measurement.

Items 12 and 15 normalize each timed batch by its round count. Their small-call figures are batch averages, rather than individually timed calls. Very short cache-hit values are sensitive to timer and loop overhead. Item 16 preserves the scan-ranking reversal between campaigns and adds a same-executable envelope check; it does not assign a stable traversal speedup to compact storage.

The probes keep production algorithms or extract checked source regions where possible. They use independent content, graph, predicate or attribute oracles before comparing performance. Item 10 records result limits and checks complete real-model prefixes, rather than presenting a shorter truncated search as a speedup. Items 13–15 have explicit numerical or rendering limits. Items 17 and 18 have no equivalent replacement implementation, so they report audited contracts and layout calculations instead of invented speedups.

Memory labels distinguish requested live/peak allocation bytes, vector-capacity payload, logical GPU payload and estimated root-arena storage. These are not interchangeable with process RSS, committed memory or VRAM. Allocation instrumentation in items 10, 11 and 16 affects absolute CPU timing. Item 10 disables its recording counters during timing; the allocation wrapper remains present. All variants within those comparisons use the same wrapper. Item 15 tracks allocation only in the added cache, and item 14's capacity experiment uses a separate synthetic allocation recorder. The separate item 19 dense-set campaign uses matched index-only allocation counters in both complete detector variants. Savings in allocation traffic can occur with no reduction in peak or retained memory.

The five OBJ models include an ordinary mixed-attribute model, large industrial and architectural models, a dense planetary mesh, and a 7.1 GB terrain file. Input sizes and SHA-256 hashes are in the raw records. A synthetic stress case is marked as synthetic and is not evidence that typical scenes have that size. No GPU shader replacement or complete application ownership migration is included in the measurements.

## Production merge boundary

The measured candidates are decision tools. None is a production implementation ready to merge. Approving an implementation means the expected gain and semantic boundary are now clear enough to start a separate change. That change must satisfy its detailed report's remaining gates and the repository's Debug build and test requirements.

Use small changes with independent validation. Shared annotation geometry can precede the persistent-settings migration. Topology slices can precede narrowing IDs. Typed graphics setters can precede a changed shared shader layout. Keep the rational oracle and guarded fallback when introducing adaptive predicates. Reject changes that require repaired input, lose source identity, or omit a required display consumer to produce their claimed saving.

## Measured effects

These figures select the results that affect a decision. Each linked report gives the workload, ranges, correctness checks, raw records and remaining gates.

| Item | Main measured or audited result | Limit on the result |
| --- | --- | --- |
| 10 | BusGameMap intersection time falls from 3,357 to 990 ms in original-index mode and 3,951 to 1,021 ms in exact-position mode. Complete 25,000-face prefixes improve by 71–86%. Allocation traffic falls by 85–87%. | Full-model runs reach the 10,000-result limit; complete prefixes and independent pair oracles also agree. Peak memory remains 106.6 MiB. |
| 11 | With one million unchanged segments and 13 retained snapshots, the single-content prototype with shared geometry changes history storage from 1,190.2 MiB to 45.8 MiB plus 30 KiB of snapshots. Recording 12 global property edits falls from 357.7 to 0.082 ms. | Synthetic upper-limit workload; geometry-only migration is not separately timed. Live and clean/save owners are outside this measurement. Distinct reshaped outlines still need distinct storage. |
| 12 | At 20,000 packets, five production uniform writes take 3.47 ms versus 0.099 ms for one isolated typed packet. A proposed integer root would save 32 arena bytes per draw root. | Batched dispatch lower bound; neither typical viewer draw count nor frame/GPU time is measured. The smaller root is a layout estimate. |
| 13 | At 32 synthetic placements, tree payload falls from 34.4 to 1.32 MiB and tree build from 421 to 11.2 ms; queries rise from 10.0 to 52.1 ms. | Candidate uses a linear instance-bound scan. Accepted extreme translation gives world/local distances of 2 and 1; ordinary rounding can also affect threshold decisions. |
| 14 | Compatible direct-stream layout calculations raise logical GPU payload by 16–92% on all five models. A separate CPU capacity trim saves 562 MiB on the terrain fixture in a median 301 ms. | Layout bytes are not measured VRAM. Direct-stream CPU fetch also regresses. Trim uses synthetic vertex contents at measured real capacities and temporarily overlaps 2,907 MiB of vertex allocations; complete import peak remains a gate. |
| 15 | An exact repeated-triangle CPU cache improves 20,000 oblique segments by 33%, but unique-triangle work regresses by 24%; at 100,000 segments the changes are -33% and +44%. Stationary submission copies 7.2 MB of CPU upload/staging data for 100,000 visible segments each frame. | Existing stationary projection already has an O(1) cache hit. No replacement shader, GPU timestamp or complete-frame gain is measured. |
| 16 | Powerplant original-index graph payload falls from 2,720 to 1,363 MiB; requested build peak falls from 3,088 to 1,544 MiB. Proved compact construction remains 13–16% slower. | Geometry stays wide. Traversal ranking reverses between compiled campaigns, so no stable scan speedup is claimed. |
| 17 | A hypothetical generic binary node raises point-tree record size from 76 to 120 bytes and annotation-tree records from 72 to 96 bytes. | Layout bounds, rather than a measured equivalent universal tree. Share utilities while retaining specialized layouts. |
| 18 | Powerplant has 1,079,616 edges with more than two uses in original-index mode, and 1,981,753 in exact-position mode. | A broad manifold repair or replacement can change the defects the application must report. No selected equivalent library adapter is timed. |
| 19 | Existing map adapters retain 80–115% more reserved index memory without a useful detector gain. The one-copy dense set leaves index peak unchanged and makes detector batches 7.3% slower on BusGameMap and 3.0% slower on Powerplant. | Dense-set timings use matched index allocation counters; peak is index-only, not application memory. Powerplant allocation traffic falls 8.8%, but peak does not. The parser-position copy is already absent. |

Start with item 10 and the shared-geometry part of item 11 when CPU latency is the priority. Item 16 is the strongest measured analysis-memory change, if its construction cost is acceptable. Item 14's thresholded capacity trim has a narrow ownership boundary and should precede a new GPU attribute layout. The first item 15 change should retain projected CPU vertices in a persistent GPU buffer; its complete-frame benefit still needs measurement. Typed setters and shared spatial utilities are smaller cleanup changes.

## Decisions supported by the investigation

The decision has two steps: select a bounded production change, then verify that change before a merge. The probe data can support the first step. They do not replace application integration tests or GPU measurements.

| Item | Implementation decision | Main condition before a merge |
| --- | --- | --- |
| 10 | Implement guarded adaptive predicates in a separate floating-point-controlled translation unit. | Keep the rational fallback and oracle; verify normal-allocator application runs and numerical boundaries. |
| 11 | Implement shared immutable annotation geometry first. Treat scene-content consolidation as a separate change. | Share both UI and saved-record geometry; preserve history, dirty state, load defaults and runtime consumers. |
| 12 | Implement typed setters with the current root layout first. | Preserve all draw variants. A smaller integer root needs C++/shader layout and rendered-output validation. |
| 13 | Consider a distance cache for repeated placement changes, with an explicit numerical contract. | Extreme accepted translations change distances. Ordinary rounding can also change arbitrary tolerance decisions; a coordinate guard alone does not prove equivalence. |
| 14 | Reject direct corner streams as a general replacement. Implement thresholded CPU capacity trim first; test a smaller indexed GPU layout on the eligible subset. | Trim on a worker with a memory-failure fallback and measured live peak. Layout changes must preserve seams, marker IDs and vertex reuse; measure GPU and complete import time. |
| 15 | Implement a persistent buffer for already-projected vertices first. Consider a repeat-aware exact CPU cache separately. | Measure complete frames and invalidate all projection inputs. Validate depth-plane behavior and clipping against the CPU reference with double intermediates. |
| 16 | Consider compact storage as a memory-versus-latency change. | Accept measured build/scan costs, prove narrowing limits and preserve defective-input records and wide fallback. |
| 17 | Share small bounds, cancellation and allocation utilities. | Keep each tree's dimensions, ordering, query semantics and measured specialized layout. |
| 18 | Reject broad replacement. Select a narrow external algorithm only for a specific gap. | Preserve defective input and provenance; include adapter memory and conversion time in its measurement. |
| 19 | Keep the current specialized analysis index. Reject the tested map and default dense-set substitutions. | A future targeted adapter must preserve insertion IDs and full findings and win its complete workload. Big-bucket and find-first sets have only microbenchmark evidence. The parser-copy clause already has zero extra saving. |

## Validation and saved evidence

The final full Debug build with `vs2026-vcpkg` succeeded with no compiler warnings. All 127 CTest entries passed, with no failures or skipped tests, in 508.78 seconds. The run includes CPU, graphics, slow-workload and probe contracts. The normal and AVX2/FMA predicate contracts both passed. Release probe builds and their contracts also passed; each detailed report states the tested candidate and its limits.

The [saved build log](benchmarks/simplifications-10-19-validation/debug-build.log), [test log](benchmarks/simplifications-10-19-validation/debug-ctest.log), [JUnit record](benchmarks/simplifications-10-19-validation/debug-ctest.xml) and [provenance manifest](simplifications-10-19-provenance.json) preserve the final check. The manifest records the base commit, configuration, file hashes and validation result. Per-experiment raw records preserve the measured inputs, samples and source or executable hashes where applicable.

The initial baseline found one existing packaging-test failure: the manifest still required the removed `cs_freeform` shader. The manifest and its test fixture now match the current shader set. The final packaging tests passed. No production optimization algorithm changed. The research probes are behind `WOBY_BUILD_SIMPLIFICATION_PROBES`, which defaults to `OFF`.

These checks validate the investigation code on this Windows machine. They do not establish Metal behavior, replacement-shader correctness, normal-allocator application performance or ownership behavior for a future production patch. Each decision above retains those specific merge gates.

## Reproduction

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_SIMPLIFICATION_PROBES=ON -DWOBY_TEST_HEADLESS=ON -DWOBY_BUILD_BENCHMARKS=ON -DWOBY_BUILD_HASH_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2
cmake --build --preset vs2026-vcpkg --config Release --target woby_simplification_10_probe woby_simplification_11_probe woby_simplification_12 woby_instance_query_probe woby_simplification_14 woby_simplification_14_capacity woby_simplification_15 woby_simplification_16_probe woby_analysis_index_probe
cmake --build --preset vs2026-vcpkg --config Release --target woby_analysis_index_dense_set_probe woby_benchmarks_dense_set_custom woby_benchmarks_dense_set
```

Then run each experiment's `run.ps1` sequentially. Build the existing `woby_benchmarks`, `woby_benchmarks_boost` and `woby_benchmarks_ankerl` targets in Release before item 19's original runner. The hash option above enables their optional dependencies. Item 19's dense-set follow-up has a separate runner and matched allocation baseline. Item 10's AVX2/FMA contract target is a separate opt-in for supported hardware: `-DWOBY_BUILD_SIMPLIFICATION_FMA_CONTRACT=ON`. Item 16 also has `run-proved.ps1` and `run-envelope.ps1`; preserve the prior outputs needed by their checks. Item 14's separate capacity probe has its own runner. Benchmark runners accept external input paths; large real models are not copied into the repository. Existing output files are preserved where a runner rejects overwrite; use a new output path for a repeat.
