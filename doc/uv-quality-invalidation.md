# UV quality preparation and invalidation

Issue [#103](https://github.com/tuncb/woby/issues/103) reported repeated 15–115 second preparations after UV metric and display-setting edits. The result signature included all UV settings, so every edit rebuilt the world mesh, analyzed every triangle, regenerated the layout and smooth normals, repeated overlap detection, sorted statistics, and uploaded all geometry.

The runtime now keeps a separate preparation signature. The full result signature still includes the settings, so a result from an obsolete worker cannot become current. CPU preparation runs on owned worker snapshots; publication and GPU upload remain on the runtime thread.

| Input change | Invalidated work | Reused work |
| --- | --- | --- |
| Source content revision, membership, transform, UV availability, scene generation, analysis type, surface/layout view, or patch separation | Full preparation | None |
| Metric | Findings, selected distribution if not cached, heatmap | Source/layout geometry, normals, edges, all geometric metric values, overlap search, other distributions |
| Area normalization | Area and stretch values derived from canonical metrics; area/minimum-stretch distributions; findings | Geometry, angle/orientation/anisotropy/overlap distributions and overlap search; heatmap unless the active metric is area or minimum stretch |
| Overlap enablement or scope | Overlap search/classification, overlap distribution, findings | Geometry and geometric metrics; heatmap unless the active metric is overlap |
| Threshold or range | Counts and findings; heatmap if highlighting changes | Geometry, metrics, overlap search, sorted percentiles and histograms |
| Near-collapse threshold | Counts and findings | Geometry, metrics, overlap search, distributions, heatmap |
| No-op setting, scene up axis, appearance or navigation | No preparation | All prepared results |

`UvQuality` retains the absolute maximum stretch and per-patch area baseline. Normalization changes derive values from these canonical values rather than repeatedly rescaling already-normalized values. Distribution caches contain small summaries, not six copies of triangle arrays. Overlap reuse preserves the original candidate count, pair order, classifications and truncation flag, including bounded partial results. Changing overlap scope or disabling/re-enabling the check invalidates that result.

A settings worker copies the last published quality result and reads the immutable prepared mesh. It does not snapshot the live scene, rebuild the layout, generate normals or edges, or copy source geometry. Successful publication replaces only quality data and, when necessary, the heatmap buffer. Source/layout vertex and index buffers retain their identity. Upload staging buffers are released afterward. Geometry changes, canceled workers and allocation failures follow the existing invalidation/recovery paths.

The remaining cost is linear in triangle count for the quality copy, findings and applicable color preparation, with sorting on the first use of a distribution. The geometry snapshot does not retain an obsolete initial quality result: quality ownership passes separately through worker publication. A settings worker temporarily holds both the published result and its updated copy. This change does not address the broader ownership and capacity issues tracked separately by #95–#98.

Regression coverage in `tests/uv_quality_tests.cpp` and `tests/uv_grid_tests.cpp` compares cached and fresh results for all six metrics, normalization round trips, overlap scope/limits, highlight statistics, colors, layout/probe identities, GPU geometry reuse, no-op edits, source changes, and stale worker publication.

## Validation and measurement method

Both Debug and Release use `vs2026-vcpkg`. The final test run executes the main suite with two workers and the updater smoke test separately. An earlier parallel run hit an unrelated updater metadata-commit failure; the isolated retry passed. The builds have no compiler warnings.

The San Miguel follow-up, measured on 2026-10-04, uses the same corpus file and UV workflow as the issue, on the Ryzen 7 5800H / RTX 3070 Laptop 8 GiB / NVIDIA 616.92 machine with approximately 63.9 GiB RAM. It is one Release Vulkan run against base commit `4abdcc3` plus this change. The comparison is the historical `7f23b975` baseline from issue #90, not an isolated paired A/B run: other intervening fixes are present. An external compiler build began during the final measurement, so its timings are indicative under CPU contention. No FPS comparison is claimed.

```powershell
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2 -E '^woby_update_helper_smoke$'
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -R '^woby_update_helper_smoke$'
cmake --build --preset vs2026-vcpkg --config Release --target woby
uv run --with psutil tests/stress/run.py build/vs2026-vcpkg/bin/Release/woby.exe D:/temp/obj_tests build/issue103-uv-san-miguel-final --suite uv --select san-miguel --operation-timeout 240 --max-private-gib 38 --min-available-gib 6
```

Use a fresh output directory for another run. The 38 GiB process-private and 6 GiB available-memory guards are retained. Measurements start after builds/tests finish. Normalization, threshold and range follow overlap mode, matching the original sequence. Ready times measure the `analysis.results` RPC after each setting is admitted; setting-admission latency is recorded separately in the [machine-readable results](benchmarks/issue103-uv-invalidation.json). Peak/last private memory comes from the 200 ms samples during the ready call; the last sample is not a settled-memory plateau.

Raw follow-up evidence is in `build/issue103-uv-san-miguel-final/uv-san-miguel-r0`: `operations.jsonl`, `resources.json`, `viewer.log`, screenshots and `case.json`. The parent manifest records executable/harness hashes, model size/time and resource guard settings. The historical case is `D:/woby/build/large-file-stress-20261003/uv-matrix/uv-san-miguel-r0`, also preserved in the issue #90 archive.

BearTrap is not remeasured here. Its independently reported heatmap-capacity failure remains outside this fix; the fast settings path requires a successful initial source upload and does not bypass capacity checks.

## Results

The final Debug checks passed: 862 tests in the main run and the updater smoke test separately. Both Debug and Release builds completed without compiler warnings.

| Edit | Historical ready time (s) | Final ready time (s) | Historical peak private (GiB) | Final peak / last private (GiB) |
| --- | ---: | ---: | ---: | ---: |
| Angle, no-op control | 0.128 | 0.195 | Not sampled | 6.99 / 6.99 |
| Area | 15.447 | 5.205 | 7.87 | 9.66 / 7.87 |
| Orientation | 11.988 | 3.223 | 7.87 | 9.88 / 7.89 |
| Anisotropy | 15.800 | 5.742 | 7.87 | 9.67 / 7.88 |
| Minimum stretch | 15.417 | 5.523 | 7.88 | 9.67 / 8.78 |
| Overlap, first search | 15.354 | 9.080 | 7.91 | 9.70 / 7.28 |
| Absolute normalization, in overlap mode | 15.530 | 2.308 | 7.93 | 8.73 / 7.26 |
| Threshold, in overlap mode | 15.681 | 3.986 | 7.94 | 9.71 / 7.94 |
| Range, in overlap mode | 15.359 | 3.534 | 7.93 | 9.70 / 7.80 |
| Cross-patch overlap scope | 16.420 | 11.311 | 7.93 | 9.72 / 7.95 |

The CPU/GPU geometry cache removes repeated preparation, but retaining it and the last published quality result while updating a worker snapshot increases peak process memory. The final maximum among these edits is 9.88 GiB, versus 7.94 GiB historically; both stay below the unchanged resource guard. Removing the obsolete initial quality result from the geometry snapshot avoided approximately 1.45 GiB of extra retention seen in the exploratory implementation.

All ten returned UV quality reports match the historical baseline exactly after stripping only the per-process namespace from object IDs. This includes all six metric statistics, histograms, findings/counts, overlap pair order and classification, candidate counts and truncation. The per-patch overlap result still reports 1,222,894 candidates and 3,890 affected triangles; cross-patch scope still reports 73,368 candidates and a truncated result. The linked-probe and later valid-triangle probe commands also succeed. The viewer log has no allocation or renderer errors.

A separate visual check used Z-up/front and a closer camera because the original workflow's front camera views the layout edge-on. Refreshed area, minimum-stretch, overlap and absolute-normalization captures are in `build/issue103-display-final/uv-display-san-miguel-r0`, alongside the exact `display-check-source.py`, RPC results and resource samples. Area and stretch show their expected different palettes; normalization in overlap mode keeps the visible heatmap while updating statistics. The check and linked probe completed without renderer errors. An earlier visual-only attempt in `build/issue103-display` ended with exit `0xFFFFFFFF` and no logged application error; the retry used a separately named, SHA-256-identical executable to isolate it from other work on the machine.
