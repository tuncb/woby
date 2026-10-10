# Separate UV measurements from layout and presentation

## Recommendation

Implement the separation in stages. First share immutable UV measurements and give layout a separate identity. Then separate overlap results and presentation results. Keep the current display mesh and 48-byte face buffer during these steps. Replacing layout geometry with UV coordinates and patch transforms should be a later change.

The main benefit is less waiting after a view or setting change. On BusGameMap, measurement and initial statistics took **783 ms**. A complete layout preparation took **1,073 ms**. A settings refresh copied **152.86 MiB** of triangle records, even when only a near-collapse limit changed. These are CPU measurements of the current implementation, not measured gains from an implemented replacement.

This investigation used production commit `7ca527f` and did not change application behavior. The first three steps have since been implemented; see the [implementation report](uv-stage-separation-implementation.md). The probe has been extended to measure that implementation. The raw results linked here preserve the original executable and source hashes.

## Current work and dependencies

`UvQuality` owns triangle measurements, UV coordinates, normalized values, overlap flags and pairs, cached distributions, settings, statistics, and finding indices. Each `UvTriangleQuality` is 152 bytes on this Windows x64 build. `shared_ptr<const UvQuality>` makes publication safe, but refresh creates a deep copy before changing its settings.

`buildComparisonWorldMesh` builds transformed surface geometry and calls `analyzeUvQuality`. For layout, it then builds a new mesh from UV coordinates. `prepareUvComparisonInputs` copies the quality result again, checks overlaps, packs face display values, and generates smooth normals. Even with overlaps disabled, that overlap call clears flags and updates statistics.

`refreshUvComparisonInputs` already retains the prepared mesh. It copies `UvQuality`, updates settings and statistics, and packs display faces only when their values can change. It preserves compatible overlap results. Initial metric distributions are cached, but finding selection and threshold statistics still scan every face.

The preparation signature excludes metric, normalization, overlap, and threshold controls. It includes surface/layout view and separated-patch mode. A view change therefore rejects the prepared source cache, rebuilds transformed geometry, and repeats measurement and overlap work. Separation also enters the signature while the surface view is active, although it has no effect on that view's positions.

Sources: [UV records](../src/uv_quality.h), [measurement and statistics](../src/uv_quality.cpp), [preparation and signatures](../src/comparison_scene.cpp), [runtime invalidation](../src/comparison_runtime.cpp).

| User operation | Current CPU work | Work that is independent of the change |
| --- | --- | --- |
| Threshold or histogram range | Copy quality; scan findings and counts; pack display faces | Measurements, overlap, mesh, cached distribution |
| Near-collapse limit | Copy quality; scan findings and counts; no color upload | Measurements, overlap, mesh, distribution, display colors |
| First use of another metric | Copy quality; build and sort distribution; scan findings; pack faces | Raw measurements, overlap, mesh |
| Normalization | Copy quality; rewrite valid face values; invalidate area and minimum-stretch distributions; update findings; pack affected colors | Raw geometry, canonical measurements, overlap, layout |
| Surface/layout or separated mode | Full source preparation, including measurements and overlap | Canonical measurements and overlap |
| Overlap enable or scope | Copy quality; run overlap scan; update findings; pack colors when overlap is displayed | Surface measurements and layout |
| Grid density, grid color mode, linked selection | No measurement preparation from these controls | Existing geometry and analysis results |

These controls already run expensive preparation on a worker. The proposed split reduces worker work and memory use; it does not move work that is currently synchronous off the UI thread. Normalization also affects the near-collapse finding count when another metric is displayed, so it cannot be treated as only a color change.

## BusGameMap measurements

The [probe](../experiments/uv-stages/main.cpp) loaded `D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj`. It selected all parts with identity transforms. The model contains **1,054,542 triangles**: **958,674 valid UV triangles** and **95,868 without UVs**, across 65 source parts. Layout contains only the 64 parts with UVs.

Results are medians of three Release samples on an AMD Ryzen 7 5800H. All samples ran in one process, in a fixed operation order, after loading. No warmup was excluded. No build or test ran during timing. Settings start with angle, per-patch normalization, and overlap disabled. Loading, GPU upload, worker scheduling, frame rate, and publication are outside the timings. The [raw results](uv-stage-separation-results.json) include all samples and model, probe, and executable hashes.

| Operation | Median |
| --- | ---: |
| Complete surface preparation | 986.3 ms |
| Measurement and initial statistics on an existing surface | 783.3 ms |
| Copy `UvQuality` alone | 31.2 ms |
| Threshold statistics/findings on an already-owned copy | 39.1 ms |
| Pack threshold display faces | 39.2 ms |
| Complete threshold refresh, angle threshold 5 degrees | 113.9 ms |
| Near-collapse refresh, limit 0.02; no display repack | 56.3 ms |
| First area-metric refresh | 269.7 ms |
| Area threshold refresh with a cached distribution | 114.5 ms |
| Area normalization switch to absolute | 304.5 ms |
| Complete layout preparation | 1,073.3 ms |
| Complete separated-layout preparation | 1,075.1 ms |
| Layout construction from an existing surface | 94.3 ms |
| Separated-layout construction from an existing surface | 94.5 ms |
| Enable per-patch overlap on an owned result | 1,095.6 ms |

The copy time is about 27% of the complete threshold refresh and 55% of the near-collapse refresh. These ratios identify an avoidable cost. They are not promised speedups: a separated implementation still needs new findings and statistics and can have different allocation and cache costs. The separately timed parts need not sum to the complete refresh.

Layout construction from an existing surface is a dependency probe. It excludes face packing, smooth normals, GPU upload, and cache publication. It also assumes that the transformed surface is retained. It is not a complete replacement pipeline. The 783 ms measurement stage is about 73% of complete layout preparation, which makes measurement reuse the first target.

The overlap result stopped at **2,000,000 candidates**, with **9,566 retained pairs**. It is partial. Separating its ownership can avoid repeated scans, but does not improve the broad phase or remove its limit. That algorithm is a separate item in the suggestions document.

| Current retained array capacity | Size |
| --- | ---: |
| UV triangle records | 152.86 MiB |
| Initial finding indices | 0.81 MiB |
| Surface display face records | 48.27 MiB |
| Transformed surface vertices, double positions, and indices | 75.68 MiB |
| Layout vertices, double positions, and indices | 50.90 MiB |
| Of layout storage: double positions reconstructed from floats | 16.02 MiB |

These figures exclude allocator overhead, metadata, source-file storage, scratch memory, and GPU memory. They are not process peak measurements. Surface and layout storage describe alternative active display meshes; they are not normally both retained as runtime caches. Sharing measurements removes the extra triangle copy during refresh, not the one canonical measurement set. Retaining both display meshes to make switching cheap would add memory. A future layout descriptor still needs UV/index access and GPU inputs, so the full 50.90 MiB is not a demonstrated net saving.

## Proposed ownership and keys

Use structs and free functions. Keep controls in `UiState` and derived data in runtime storage. Each published result must retain the immutable inputs that its indices refer to.

| Product | Data and dependencies |
| --- | --- |
| UV measurements | Canonical face values, UV coordinates, validity/orientation, source part and triangle references, patch totals. Key: selected source instances, source revision, transforms, UV availability, and patch identity. No view or presentation controls. |
| Overlap result | Face flags, ordered pairs, counts, checked/truncated state. Key: measurement identity, overlap scope, and work limits. No layout or normalization controls. |
| Distribution summaries | Percentiles, histogram, and total valid surface area. Key: measurements, metric, relevant normalization, and overlap result for the overlap metric. |
| Presentation findings | Finding indices, threshold/range counts, near-collapse count, area percentages, and the settings used. Key: measurements, overlap, metric, normalization, and selection controls. |
| Layout | Frame, patch extents and placements, source-to-display mapping, and bounds; initially a materialized display mesh. Key: source/display bounds, UVs, patch selection/order, view, and separation when in layout view. |
| Display values | Current compact face values and their layout mapping. Key: relevant presentation and display geometry. Preserve the existing conditional upload behavior. |

Start with the current conservative source identity for measurements. Rigid transforms could permit more reuse, but nonuniform scale changes angle and stretch measurements, and physical surface area affects reports. Transform-specific reuse is a separate optimization. Overlap can safely use the measurement identity initially even though it needs fewer inputs.

Store canonical area log ratio and singular values once. For patch baseline `b`, derive area as `absoluteAreaLog2 - b` and stretch as `absoluteStretch / exp2(b / 2)`. Absolute mode uses `b = 0`. Calculate factors once per patch. Do not repeat logarithms and exponentials for each face on every normalization change. Preserve the existing operation order where it affects values at thresholds, and compare repeated normalization changes with the current implementation.

Keep the small distribution summaries across view changes. Cache area and minimum-stretch summaries by normalization if repeated switching justifies it. A first uncached distribution still needs a scan and sort; ownership separation does not remove that cost. Do not add a sorted per-face cache for every metric in the first change.

## Benefits and costs

The split removes bulk measurement copies from presentation refresh and avoids repeated measurement after layout changes. It also makes invalidation easier to test: a range change cannot change raw measurements, and a layout change cannot change overlap pairs. A worker can keep an old immutable result alive while preparing a new selection without duplicating all raw records.

The cost is more explicit dependencies. Workers must publish only matching generations. A small result must not retain obsolete source meshes through an accidental ownership chain. Cache retention must be bounded, particularly if both surface and layout meshes are kept. Separate result arrays require stable face IDs and matching lengths; missing-UV faces remain in reports but are absent from layout.

On-demand normalized access adds arithmetic and a patch lookup to a read. Removing bulk rewrites should help normalization changes, but random probe reads and repeated CPU scans need measurement after implementation. CPU statistics must continue to use double precision. Moving thresholds directly to a shader risks a CPU/GPU mismatch at strict boundaries, and the current clamped color value cannot reconstruct the original metric. Keep CPU highlight flags first. Shader-only presentation can be evaluated later with exact edge-case checks.

Replacing the layout mesh has the largest integration cost. `ScenePickPart`, linked patch selection, finding focus geometry, display normals, and triangle drawing currently use the mesh. Camera framing already has a path that avoids building another mesh; preserve its bounds behavior. A virtual layout needs a CPU accessor with the same float rounding, missing-UV exclusion, patch placement, source identity, and triangle order as the renderer. It also needs equivalent edge drawing and picking inputs. Control API triangle values and source-space probe positions must remain unchanged.

Sources: [layout math](../src/uv_analysis.h), [picking](../src/scene_pick.cpp), [finding geometry](../src/uv_quality.cpp), [control API](../src/control_uv.cpp), [GPU uploads](../src/comparison_gpu.cpp), [existing UV contract tests](../tests/uv_grid_tests.cpp).

## Recommended implementation order

1. **Share immutable measurements.** Introduce canonical metric access and a separate presentation result. Keep the existing display mesh and compact face format. Remove the deep triangle copy from setting refresh and initial overlap preparation. Test normalized values, invalid faces, strict threshold/inclusive range rules, histograms, finding order, API output, and cancellation.
2. **Separate source/measurement and layout identities.** Reuse measurements across surface/layout and separation changes. Keep bounded ownership of source inputs and the active display. Do not retain both full meshes without measuring the memory cost. Test repeated view changes, source edits, transforms, patch isolation, and stale worker completion.
3. **Separate overlap results and distribution dependencies.** Reuse compatible results across presentation and layout changes. Preserve original UV coordinates, patch-domain rules, candidate/pair ordering, work limits, and partial-result status. Disabled overlap must still appear unchecked to users; a retained cached result must not leak flags into active findings. Do not change the broad-phase algorithm in this step.
4. **Reduce display repacking where measurements support it.** Keep near-collapse and grid changes cheap. Evaluate splitting selected values/highlights from repeated normals or moving suitable controls to uniforms. Compare upload volume and CPU/GPU output before choosing a new face format.
5. **Evaluate virtual layout separately.** Add the CPU position accessor and matching render path before removing `uvLayoutMesh`. Measure frame rate, picking, framing, switching latency, and peak memory. Keep the materialized layout if the added indirection outweighs its savings.

Steps 1–3 are the recommended first implementation scope. Steps 4–5 need separate performance and display checks; the current CPU probe does not establish their net benefit.

## Reproduction and validation

Enable `WOBY_BUILD_BENCHMARKS` in the local `vs2026-vcpkg` configuration, then run:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_uv_stages_probe
./experiments/uv-stages/run.ps1 -Model 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj'
```

The probe verifies layout indices, float positions, double positions, UVs, and source/measurement mapping against full preparation. A small in-memory fixture covers missing and collapsed UVs, shared UV coordinates across patches, separated layouts, overlap enable/disable, immutable refresh, and cancellation. Timing uses the existing APIs; no alternative production implementation is substituted.

Validation completed on 2026-10-10: the full Debug build and the Release probe build passed without compiler warnings. All 79 Debug unit test groups passed, including the new probe contract and eight graphics groups. `git diff --check` passed. No application source files were changed.
