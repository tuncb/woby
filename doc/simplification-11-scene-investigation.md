# Item 11: scene content and history

The [shared geometry implementation](shared-annotation-geometry-implementation.md) completes the first production step, with owner, persistence, history, reload and worker checks. Its production-path measurements are separate from the prototype results below.

This investigation uses production commit `46a27585c5bc8638a8bde5d601e5cead71376631`. It adds isolated probes. It does not change scene state, persistence, or undo behavior in the application.

## Decision scope

Implement immutable annotation geometry first. Then use one typed persistent-content snapshot for history and dirty checks. These changes have separate value and can be reviewed separately. Keep file geometry out of history. Do not use a revision or hash as the only content-equality check.

The measured geometry saving is large enough to justify implementation. The broad content change also reduces retained metadata, but this prototype does not improve every dirty check. At 16,384 groups, its clean dirty query is 7.9% slower than production. Complete the settings boundary and the integration tests below before a production merge. Do not merge the experimental metadata shell as the final content type.

A scene arena or a parent-ID hierarchy is a separate decision. This experiment does not establish a need to replace the current hierarchy. Existing scene query caches already remove many repeated traversals; see [scene query caching](scene-query-performance.md).

## Current work

[`SceneSnapshot`](../src/scene_history.h) owns both a metadata-only `UiState` and a `SceneDocument`. On each notified edit, [`recordSceneHistory`](../src/scene_history.cpp) builds a document and an identity vector before checking equality. If content changed, it also copies the metadata into the snapshot. Both representations own the annotation segment vectors. [`updateSceneDirty`](../src/ui_operations.cpp) performs another document conversion and equality check. The main loop calls that function after a notified history update.

Idle history frames already skip traversal and allocation when `sceneEditRevision` is unchanged. Ordinary camera changes do not create a revision. The opportunity concerns edits, notified no-ops, and explicit dirty checks. It does not provide a new idle-frame cache benefit.

The duplication also includes group settings, file settings, hierarchy settings, paths, labels, comparisons, and saved views. Geometry ownership improvements have already made a published `Mesh` copy cheap. They do not remove the separate annotation geometry values.

The duplicated types have different defaults. For example, `UiGroupState` starts with triangle and vertex display off, while `SceneGroupSettings` starts with both on. `UiState::showOrigin` and `SceneDocument::showOrigin` also differ. A common settings type must retain explicit UI initialization and version-specific load defaults. Legacy comparison-membership fields belong in the read adapter, not the new canonical group settings.

The current equality comparison starts with annotations. An unchanged annotation outline can therefore be scanned before a later global setting is found to differ. A pointer-equality fast path on a shared immutable outline is safe. Distinct outline owners must still compare by value: a drag that returns to its first value must remove the coalesced undo action.

The optional hierarchy change has a different trigger. [`findSceneObject`](../src/scene_objects.cpp) visits annotations, comparisons, folders, files, and groups. [`removeFileFromState`](../src/ui_operations.cpp) calls it for each selected ID after removal. For `S` selected IDs and `N` remaining objects, this cleanup can perform `O(S N)` ID visits. It also repairs positional file indices in the existing tree. A revisioned ID-to-location table, or one ID set for this cleanup, can address that work without replacing persistent hierarchy storage.

[`ScenePartRecord`](../src/scene_queries.h) retains ancestor IDs for selection queries. Its ancestor payload is eight bytes per stored entry, before capacity and container overhead; it includes the part's own ID. Parent links could reduce repeated ancestor storage, but a query would then need to walk those links or use an equivalent derived cache. These are representation and complexity bounds, not measured savings. The history fixtures below cover flat scenes and do not measure structural edits or a deep hierarchy. The decision for a full scene arena is therefore to defer it; the measured duplicate-history cost supplies a narrower implementation with a known benefit.

## Prototype contract

The [source](../experiments/simplification-11/main.cpp) measures three representations:

| Representation | History content | Annotation ownership |
| --- | --- | --- |
| Production | Existing `SceneSnapshot` | Two geometry values per snapshot |
| Single content | One logical metadata owner with reload descriptors | One geometry value per snapshot |
| Shared content | Same metadata owner | Shared immutable geometry published before the edit |

The candidate uses structs and free functions. It retains IDs, logical settings, labels, paths, hierarchy, group reload descriptors, and annotation references. It omits session values and mesh assets. The metadata shell uses existing types to isolate the storage decision; it is not the final persistent-settings API. Save conversion and production `sceneContentEqual` are used as an independent logical-content oracle outside the timed region. This retains production's rule that ordinary live camera navigation does not dirty scene content; it is not a serialized-file round-trip test.

The self-test checks notified no-ops, coalescing, a drag that returns to its first value, branch deletion after undo, geometry replacement, equal geometry with different owners, session-value exclusion, historical owner lifetime after removal, and persistence equality. It refuses Apply View transactions, because those need an additional before/after navigation record. The experiment does not implement runtime undo, source reload, worker publication, or cache invalidation.

## Measured results

[Raw measurements](simplification-11-results.json) contain five Release samples per workload. The order of the three history variants alternates. Each sample resets history, makes 12 separate global point-size edits, and retains 13 snapshots. All 30 samples pass the persistence oracle for every retained shared-content snapshot against production. The single-content timing variant uses the same capture function with a copied geometry owner; these runs check its snapshot cardinality and capacities, rather than independently converting every single-content snapshot. Import, fixture construction, publication of immutable geometry, and oracle conversion are outside the timed region.

Times below are medians in milliseconds. Live memory is requested C++ allocation storage retained by the measured history, in MiB (`1 MiB = 1,048,576 bytes`). The shared-history column excludes its one published geometry owner; that payload is reported separately.

| Workload | History time: production / single / shared | Retained history MiB: production / single / shared | Shared geometry payload MiB |
| --- | ---: | ---: | ---: |
| 256 groups; no annotation | 2.146 / 0.841 / 0.873 | 1.906 / 1.239 / 1.239 | 0 |
| 1 group; 20,000 segments | 8.458 / 3.230 / 0.042 | 23.855 / 11.935 / 0.029 | 0.916 |
| 1 group; 1,000,000 segments | 357.710 / 121.441 / 0.082 | 1190.237 / 595.126 / 0.029 | 45.776 |
| 16,384 groups; no annotation | 106.500 / 23.709 / 23.037 | 117.578 / 78.020 / 78.020 | 0 |
| `projected-ranges.woby`; 147 segments | 0.262 / 0.174 / 0.046 | 0.236 / 0.129 / 0.034 | 0.007 |
| BusGameMap; 65 groups; no annotation | 1.227 / 0.392 / 0.454 | 0.610 / 0.386 / 0.386 | 0 |

Production and single-content segment capacities follow the storage model exactly. At one million segments, production retains 1,248,000,000 segment-array bytes; single content retains 624,000,000. Shared history retains no additional segment array per snapshot and uses one owner with 48,000,000 segment-array bytes. Including this payload, the modeled history geometry saving is 96.2%. Owner metadata and the shared-pointer control block add a small constant allocation outside this measurement. The remaining shared snapshot allocation is 30,512 bytes. The current live value and clean/save values are outside this snapshot comparison; moving them to the same owner is part of the implementation gate.

The geometry gain also holds for a coalesced global point-size drag with unchanged annotation geometry. This 12-frame property drag retains two snapshots, but production still replaces large geometry copies on every frame. It does not reshape an annotation or measure geometry publication.

| Workload | Global point-size drag: production / single / shared ms | Production retained / peak MiB | Single retained / peak MiB | Shared retained / peak MiB |
| --- | ---: | ---: | ---: | ---: |
| 20,000 segments | 4.430 / 0.753 / 0.020 | 3.670 / 5.504 | 1.836 / 2.754 | 0.005 / 0.006 |
| 1,000,000 segments | 423.157 / 148.116 / 0.034 | 183.113 / 274.669 | 91.558 / 137.336 | 0.005 / 0.006 |
| 16,384 groups | 93.604 / 27.287 / 26.534 | 18.089 / 27.132 | 12.003 / 18.004 | 12.003 / 18.004 |
| Real annotation scene | 0.166 / 0.044 / 0.024 | 0.036 / 0.053 | 0.020 / 0.029 | 0.005 / 0.007 |
| BusGameMap | 0.772 / 0.245 / 0.201 | 0.094 / 0.140 | 0.059 / 0.089 | 0.059 / 0.089 |

The shared drag memory also excludes the same published owner. At one million segments, the median of each sample's slowest discrete history-record call is 29.193 ms in production, 9.900 ms in single content, and 0.019 ms in shared content. These are history calls for global property edits, not full frame times. Coalescing limits retained history size; it does not remove copying cost. A reshape publishes a distinct geometry owner and still needs storage for distinct historical outlines; the unchanged-owner times above do not predict that publication cost.

| Workload | 10 notified no-ops: production / single / shared ms | 10 clean dirty queries: production / shared ms |
| --- | ---: | ---: |
| 256 groups | 0.760 / 0.465 / 0.472 | 0.472 / 0.482 |
| 20,000 segments | 2.269 / 2.253 / 0.024 | 2.227 / 0.026 |
| 1,000,000 segments | 212.174 / 212.222 / 0.038 | 211.344 / 0.038 |
| 16,384 groups | 61.117 / 43.386 / 43.539 | 39.882 / 43.025 |
| Real annotation scene | 0.070 / 0.048 / 0.036 | 0.051 / 0.021 |
| BusGameMap | 0.480 / 0.235 / 0.222 | 0.253 / 0.219 |

Single content alone does not remove the no-op geometry copy: both its one-million-segment no-op cost and production's cost are about 212 ms for ten calls. Immutable sharing removes that copy and the value scan when the owner is unchanged. In production, these ten no-ops cumulatively request 480,024,630 bytes and peak at 48,001,983 extra bytes. The shared candidate requests 16,640 bytes cumulatively and peaks at 1,664 extra bytes.

The metadata-only dirty-query regression is real in this probe. Production builds lean save records. The prototype captures existing UI group structs, which contain derived metadata. At 16,384 groups it peaks at 6,292,389 extra requested bytes, compared with production's 4,768,037. A final persistent-content type must omit these derived fields. Measure it before claiming a universal dirty-check improvement.

All variants allocate zero bytes across 10,000 idle history calls. Production's usual median is about 0.092 ms for those calls. Differences of tens of microseconds in this already skipped path do not justify a migration. Small real-model timings also vary: BusGameMap production history ranges from 0.756 to 1.341 ms, and shared history ranges from 0.174 to 0.487 ms. Use the storage result and large-outline latency as the primary decision evidence.

## Memory model and limits

`AnnotationSegment` is 48 bytes on this x64 build. For `H` retained snapshots and `S` unchanged segments, production stores `96 H S` segment-array bytes. Single content stores `48 H S`. Shared content stores `48 S` once, plus one shared pointer per annotation per snapshot. The live outline and clean/save document can add more copies. The probe reports snapshot storage separately from its pre-published geometry owner.

One million segments are the accepted upper limit for one annotation. The limit applies per annotation, not to the scene. History currently has no 100-action cap: existing tests require older metadata actions to remain available. A color or global-property edit can therefore multiply a large outline repeatedly. Immutable sharing fixes unchanged outlines; it cannot remove the storage of distinct historical outlines after repeated reshaping.

Sharing also needs a publication rule. Current reshape, creation, load, and coordinate-rebase operations assign geometry values. They must construct a new validated owner and publish it atomically. Rendering and query consumers must read the owner. A mutable alias would make an earlier undo snapshot change when the live outline changes.

## Implementation gates

1. Give `UiAnnotation` and in-memory `SceneAnnotationRecord` a shared immutable geometry owner, or put the immutable bulk arrays behind a common owner inside `AnnotationGeometry`. Changing only `UiAnnotation` would leave the save-record copy in each snapshot and dirty conversion. Serialize the same `.woby` fields by reading the owner; publish a new owner at operation/load boundaries. Preserve full value equality when owners differ. Do not make this owner retain mesh assets or runtime caches.
2. Keep attachment validity, pending status, geometry revision, and derived world/projection lines outside persistent content. Current history copies some session fields with the annotation/comparison metadata; a typed-content boundary should make these exclusions explicit.
3. Extract persistent group/file/folder settings, with adapters for `.woby` positional references. Preserve canonical `UiState` ownership and operation-boundary validation.
4. Replace the dual snapshot with one typed content owner. Keep group name and primitive-kind descriptors for removed-file reload validation. Preserve identities for missing annotation and comparison targets.
5. Use that same content equality for the clean baseline and history. Keep saved-view cameras in content, live navigation outside content, and Apply View as an explicit navigation transaction.
6. Run existing history, save/load, annotation, view, and worker cancellation tests. Add tests for independent owner equality, shared outline replacement, removal/reload, unchanged target references, no-op drags, failed batch reload, missing inputs, and late worker results.

For a merge, require equivalent content, IDs, persistence mapping, dirty state, and undo grouping. Measure an edit plus the subsequent dirty query in the application. The isolated probe gives a storage and edit-cost decision; it is not an end-to-end frame-time measurement.

## Reproduction

Configure the repository with `vs2026-vcpkg` and `-DWOBY_BUILD_SIMPLIFICATION_PROBES=ON`. Build `woby_simplification_11_probe` in Debug and Release. Run the `woby_simplification_11_contract` CTest entry. Then run [the measurement script](../experiments/simplification-11/run.ps1) with an idle machine. Its default real model is `D:/temp/obj_tests/uploads_files_2720101_BusGameMap.obj`; use `-Model` to supply another path.

Requested live, peak, and cumulative C++ allocation bytes come from a global allocation wrapper. They exclude allocator headers, driver allocations, process working set, and RSS. Array capacity bytes are reported separately. Allocation accounting adds a header and atomic counters to each allocation; these timings apply to the instrumented executable. Relative results compare the same instrumentation.
