# Analysis geometry, incidence, findings, and display

The implementation follows steps 1 to 4 of the [investigation](analysis-stage-separation-investigation.md). Step 5 is the compatibility review below. The different identity and degeneracy rules require a separate indexing change.

## Geometry and incidence

`buildAnalysisGeometry` validates sources, transforms coordinates as doubles, assigns vertex identities, removes exactly collapsed faces, and preserves canonical source and face order. Its immutable result has no topology edges, adjacency, components, or findings. Intersections read this result directly.

`buildTopologyIncidence` adds the graph to the same geometry owner. Geometry records contain 24 bytes per position and 48 bytes per face. Incidence records contain spans, face edges, and component IDs. Source point references and their offsets remain with geometry so a later topology request can use the same owner without transforming or welding again.

The comparison runtime retains one compatible geometry product per side. The cache checks the immutable source-input owner, coordinate origin, and topology mode. Source inputs include selected ranges, batch/source identities, transforms, and source data. The runtime also checks its existing scene geometry signature before publication. A source or mode change releases the cached graph and geometry; a budget change retains them. An intersection job captures geometry without the incidence owner. Publication retains any compatible graph already held by the current result. The job cannot retain an obsolete result in the current cache.

Cold intersection requests no longer construct incidence, topology findings, or topology overlays. Requests after topology reuse geometry. A topology request after intersections builds incidence over that geometry. An intersection rerun still builds its BVH and runs exact tests.

## Finding dependencies

Topology findings have four internal completion bits:

| Finding family | Work |
| --- | --- |
| Edge findings | Boundary edges, non-manifold edges, winding findings |
| Vertex links | Non-manifold vertices |
| Boundary regions | Hole candidates and threshold selection |
| Fin patches | Fin candidates and threshold selection |

The runtime requests the families needed by queued detectors. Later requests reuse incidence. Publication merges only the families produced by the worker and preserves earlier findings and independent detector states. Manual runs, cancellation, known counts, export, and navigation use the existing control API.

The public full-topology function still computes all four families. Separate family runs must produce the same ordered reports as that function. Source identity, batch boundaries, transforms, double coordinates, exact collapsed-face rules, and intersection candidate limits are unchanged.

## Display work

Duplicate, degenerate, and intersection display arrays are now prepared on the analysis worker. GPU upload consumes those arrays. It does not traverse the findings to construct overlays. GPU-only arrays are released after a successful upload; bounds and edges used for navigation and picking remain available.

Hole and fin threshold changes use a separate display worker. This worker captures immutable finding snapshots and shared graph owners. It copies and filters the required finding family on the worker, then prepares bounds and display geometry. Unchanged families are omitted. The UI thread only selects snapshots, checks completion, and publishes the result.

Publication checks the source signature, the base topology revision, and both thresholds. A changed source, topology mode, finding revision, or threshold cancels obsolete display work. A failed display job keeps the previous findings and reports failure; an explicit request can retry. Each successful display publication has its own revision. Show flags do not trigger this work. Bounds remain available when an overlay is hidden.

GPU allocation remains eager for completed stages. Broad lazy allocation was conditional on a separate visibility-latency measurement in the recommended order. It is not enabled here. The remaining topology upload stage can still upload unchanged families together.

## Cost and scope

Retained intersection geometry uses more idle memory than retaining intersection findings alone. It removes the full graph from cold intersection work and avoids repeated geometry preparation. Geometry also retains point provenance for topology upgrades, so its actual size is larger than the investigation's minimum position/face estimate.

Immutable hole and fin snapshots add retained finding arrays. They keep large snapshot copies off the UI thread. They share geometry and incidence; they do not duplicate the graph. Full topology still needs incidence and findings, so the intersection-only memory reduction does not apply to all checks together.

The scheduler prevents new topology detector builds from overlapping an intersection or threshold-display worker in the same comparison. Source, quality, and distance requests can still proceed while intersections run, as required by the existing runtime behavior.

## Step 5: common indexing review

A common index for legacy diagnostics, surface quality, and source topology needs more than a shared vertex map.

| Rule | Source analysis geometry | Legacy diagnostics and quality |
| --- | --- | --- |
| Identity | Original source ID plus transform, or exact transformed double position within a source batch | Geometric position within a render analysis batch |
| Eligible faces | Exclude exactly collapsed triangles | Use a numerical degeneracy threshold, including `crossSquared <= maxEdgeSquared² * 1e-24` |
| Face references | Original file, part, and source triangle IDs | Render face order and quality neighbor relationships |

The new source geometry can retain a very thin triangle that quality classifies as degenerate. Original-index mode also keeps coincident source IDs separate. Replacing either legacy index with this geometry would therefore change counts or adjacency. A future common index must accept explicit identity, batch, eligibility, and face-mapping policies and prove parity for each consumer.

The world render mesh remains necessary for on-screen comparison, picking, dimensions, and current quality paths. Detector-only source snapshots can be a separate change. This implementation does not remove the render mesh or merge incompatible degeneracy rules.

## Validation and measurements

The probe covers both topology modes, duplicate and collapsed faces, missing source records, transformed instances, separate source batches, ordered findings, pair/candidate budgets, and cancellation. Unit tests cover geometry ownership through an upgrade, separate finding reports, cache-key changes, stale stage publication, hidden navigation bounds, display geometry, stale display workers, and failure/retry.

The full Debug build and the Release probe build completed without warnings. All **77 unit test groups** passed, including the CPU and graphics groups. `git diff --check` passed.

### BusGameMap results

The same BusGameMap fixture contains 1,054,542 render faces and 1,054,306 surviving analysis faces. Each mode ran three samples in a Release process on the Ryzen 7 5800H. The baseline executable was retained from the initial investigation; its SHA-256 matches that investigation's record. The baseline ran first, then the implementation. No build or test ran during measurement. Loading and result-signature comparison are outside the timed regions. See the [raw measurements and hashes](analysis-stage-separation-implementation-results.json).

| Median time | Original index | Exact position |
| --- | ---: | ---: |
| Baseline intersections-only stage | 4.235 s | 4.908 s |
| New cold intersections-only stage | 3.552 s | 4.104 s |
| Cold reduction | **16.1%** | **16.4%** |
| New rerun using cached geometry | 3.437 s | 3.765 s |
| New intersections after topology | 3.504 s | 3.712 s |
| Reduction after topology, versus baseline stage | **17.3%** | **24.4%** |
| Build geometry alone | 0.134 s | 0.325 s |
| Baseline full topology builder | 0.841 s | 1.194 s |
| New full topology builder | 0.762 s | 1.110 s |
| Upgrade cached geometry to comparison topology stage | 1.010 s | 1.151 s |
| Threshold display preparation on worker | 9.36 ms | 6.26 ms |

The topology-stage upgrade includes legacy diagnostics and display preparation; it is broader than the full topology builder. The threshold measurement changes the hole ratio to 1 and the fin ratio to 0.5. It measures worker preparation, not UI latency or GPU upload.

Every new input path produced the same complete ordered intersection signature within each sample. This comparison includes face IDs, geometry, provenance, source/batch identity, availability, candidate counts, affected faces, and budget status. Baseline and implementation runs also retained the same reported count and candidate count: 10,000 findings; 538,195 candidates in original-index mode and 576,805 in exact-position mode. Both stopped at the pair limit. These are bounded analyses, not unlimited intersection scans.

| Retained array capacity | Original index | Exact position |
| --- | ---: | ---: |
| Baseline full topology arrays | 308.59 MiB | 309.11 MiB |
| New intersection geometry, including point provenance | 72.80 MiB | 72.80 MiB |
| New full topology arrays | 312.66 MiB | 313.15 MiB |
| Additional immutable display-finding snapshots | 8.48 MiB | 8.47 MiB |
| Unrequested topology overlays in an intersection job | **0 bytes** | **0 bytes** |

The intersection geometry uses about **76% less array storage** than the old topology payload. Full topology arrays increase by about 4 MiB for provenance offsets; display snapshots add about 8.5 MiB. These figures exclude the original source mesh, container metadata, allocator overhead, scratch storage, and the intersection BVH. They are not total process-memory figures.

The runner also sampled Windows `PeakWorkingSet64` every 200 ms. The complete baseline probe reached 831.64 MiB; the complete implementation probe reached 858.86 MiB. The probes retain a full topology result while timing other paths, and the new probe also retains upgrade/display results. Their peak workloads differ. These figures describe the whole probe process and do **not** establish a reduction in isolated cold-request peak memory or private committed memory.

### Reproduction

```powershell
$env:VCPKG_ROOT = 'C:\work\vcpkg'
$env:VULKAN_SDK = 'C:\VulkanSDK\1.4.357.0'
cmake --build --preset vs2026-vcpkg --config Debug
ctest --test-dir build/vs2026-vcpkg -C Debug -L unit --output-on-failure -j 4
cmake --build --preset vs2026-vcpkg --config Release --target woby_analysis_stages_probe
./experiments/analysis-stages/run.ps1 -Model 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj'
```

The probe is enabled by `WOBY_BUILD_BENCHMARKS`. The runner accepts `-Executable` and `-OutputDirectory` for a retained baseline executable. The saved baseline binary is a local build artifact; the repository retains its hash and measured results. BearTrap was not measured in this implementation run.
