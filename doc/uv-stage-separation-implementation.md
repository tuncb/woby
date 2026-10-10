# UV measurement, layout, and presentation separation

## Changes

The first three steps from the [investigation](uv-stage-separation-investigation.md) are implemented. Settings and layout changes can now reuse the same immutable UV measurements. The application still builds one active display mesh and uses the existing 48-byte GPU face records.

| Area | Change |
| --- | --- |
| Measurement ownership | `UvMeasurements` owns canonical face values, UVs, patch normalization factors, and validity counts. `UvQuality` shares this immutable object. Copying a presentation result no longer copies its triangle array. |
| Normalization | `uvTriangleQuality` derives normalized area and stretch from canonical values. Patch factors are computed once. The redundant absolute-maximum-stretch field is removed. |
| Overlap ownership | `UvOverlapResult` owns flags, pairs, counters, limits, scope, and measurement identity. Presentation changes share this result. One compatible result can remain cached while overlap is disabled; disabled findings and reports remain unchecked. |
| Distributions | Area and minimum-stretch summaries have separate absolute and per-patch cache entries. Other metric distributions share one entry. Overlap changes invalidate the overlap distribution. |
| Layout identity | `comparisonMeasurementSignature` excludes layout and presentation controls. View changes reuse measurements and overlap results while rebuilding the active display mesh. Separation changes in surface view no longer invalidate that mesh. |
| Lifetime | A source edit clears the retained analysis cache. A worker publishes only if its request still matches. Inactive display meshes are released; the cache does not keep both surface and layout meshes. Allocation failure also releases retained UV results. |
| Consumers | Display values, finding navigation, reports, and control API pages use the normalized triangle accessor. Source IDs, triangle order, original UV coordinates, and control API field names remain unchanged. |

Primary code: [UV types](../src/uv_quality.h), [measurement and presentation](../src/uv_quality.cpp), [overlap](../src/uv_overlap.cpp), [preparation](../src/comparison_scene.cpp), and [runtime ownership](../src/comparison_runtime.cpp).

Statistics and compact face preparation read only the needed scalar values. They do not construct a complete resolved triangle for each face. The full accessor remains available for finding navigation and control API pages. The expanded display reference path also uses it, so tests compare the compact path with a separate value-resolution path.

## BusGameMap comparison

The Release probe loaded the same 1,054,542-face model used in the investigation. These are medians of three samples after loading, on the same Ryzen 7 5800H. No build or test ran during measurement. The [baseline](uv-stage-separation-results.json) and [implementation results](uv-stage-separation-implementation-results.json) retain all samples and model, source, and executable hashes.

| CPU operation | Before | After | Change |
| --- | ---: | ---: | ---: |
| Layout preparation after an existing UV result | 1,073.3 ms | 252.7 ms | 76.5% less |
| Separated-layout preparation after an existing UV result | 1,075.1 ms | 254.0 ms | 76.4% less |
| Angle threshold refresh | 113.9 ms | 81.7 ms | 28.3% less |
| Near-collapse refresh | 56.3 ms | 22.8 ms | 59.6% less |
| First area-metric refresh | 269.7 ms | 258.2 ms | 4.3% less |
| First switch to absolute area normalization | 304.5 ms | 243.5 ms | 20.0% less |
| Copy presentation result | 31.23 ms | 0.29 ms | 99.1% less |
| Cold surface preparation | 986.3 ms | 906.0 ms | 8.1% less |

The layout comparison uses the full CPU preparation API on both sides. The old runtime had to prepare from scratch after a view change. The new runtime supplies compatible measurements and overlap results. The new probe verifies the shared measurement owner and compares layout positions, UVs, indices, provenance, and findings with fresh preparation. It keeps the surface and cold layout resident during the cached-layout timing; the runtime does not retain both display meshes.

A return to the cached per-patch area distribution took **86.0 ms**. The original probe did not time that exact return operation, so there is no matching before value for it. New cold layout preparation still took **993.9 ms**; the large view-switch gain depends on compatible measurements already being available.

The isolated face-packing stage is a tradeoff: **39.2 ms before, 46.9 ms after**, or 7.7 ms more. Complete threshold refresh is still faster because the large record copy is gone and the statistics loop does less repeated work. The first area refresh remains dominated by distribution work, so its small measured improvement should not be treated as a precise speedup guarantee.

Raw triangle records decreased from **152 to 144 bytes**: **152.86 to 144.82 MiB** for this model. Refresh shares that array instead of allocating a second copy. Finding indices remain owned by the presentation result and can still be copied. Overlap uses a separate one-byte flag array per face, plus its bounded pair list. One compatible overlap result can remain cached when checking is disabled. These are array storage figures, not process peak-memory measurements.

Both overlap scans retained **9,566 pairs** and reached the **2,000,000-candidate limit**. Both results are partial. This implementation changes reuse, not overlap completeness.

All timings exclude file loading, GPU upload, worker scheduling, and publication. They do not measure frame rate or total click-to-display latency. The operation order is fixed, and small differences can include allocator and cache effects.

## Limits

Threshold changes still scan faces to produce findings and counts. Color changes still pack and upload the compact face buffer. A first uncached distribution still sorts values. Layout changes still build transformed geometry, layout positions, normals, and GPU inputs.

Overlap enumeration, candidate order, exact tests, and work limits are unchanged. A cached partial result remains partial. Only one overlap result is retained; a scope change rebuilds it. Removing the physical layout mesh or moving highlight decisions into shaders remains a separate change.

## Validation

New and extended unit tests check shared measurement ownership, normalization round trips, strict thresholds and inclusive ranges, disabled overlap behavior, bounded overlap reuse, distribution variants, source identity, layout reuse, inactive mesh release, and stale worker rejection after a source edit.

The existing tests continue to compare all six metrics, face values, normals, missing and collapsed UV handling, source provenance, finding navigation, and control API output. Test fixtures use in-memory meshes or the existing unique temporary roots.

- Full `vs2026-vcpkg` Debug build: passed without compiler warnings.
- Release probe build: passed without compiler warnings.
- All 79 Debug unit test groups: passed.
- All seven UV viewer checks: passed. These cover UV grid and layout rendering, all six quality metrics, linked picking, source isolation, and scene save/reload.

The final scalar-access change was followed by another full unit run and repeated layout-quality and six-metric viewer checks. All passed. The Debug test executable needed a fresh link after an incremental-link error (`LNK1163`); rebuilding with the same preset and `/p:LinkIncremental=false` succeeded without warnings. This was a local build override, not a repository configuration change. `git diff --check` also passed.
