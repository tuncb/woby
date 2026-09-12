# Issue #41 implementation plan

Status: duplicate-point and source-ID duplicate-triangle implementation added on
2026-09-12. Remaining detectors and the complete shared-topology milestone are open.

The first slice preserves OBJ position records, STL corners, and importer vertex
buffers; adds per-analysis exact source checks, grouped navigation/highlighting,
Run/Show controls, scene version 8, and bounded CLI findings. Triangle IDs identify
generated triangles; original polygon provenance remains follow-up work. Detection
is per source file over selected parts, including unused points for whole-file
inspection. STL corner repetitions are informational and its source-ID triangle
check is unavailable. Legacy geometric counts retain their meanings.

Run changes currently invalidate the shared analysis job; Show changes do not.
Separating distance/topology/detector job caches remains part of later scheduling
work. JSON findings are bounded to 100 groups and 100 members per group; full
navigation is available in the UI. Performance budgets remain to be established.

Issue: [More explicit detectors](https://github.com/tuncb/woby/issues/41).
Baseline inspected: `9463124`, 2026-09-08. Recheck the baseline before implementation.

## Outcome and scope

Provide explicit, selectable mesh findings with documented definitions, per-detector
settings, source element references, and consistent UI, CLI, and screenshot output.
Inspection works with either analysis side populated; distance measurements still
require both sides. Inspection does not repair or otherwise change input geometry.

Deliver all ten requested detector categories in stages. Fins are candidate
classifications; non-partitioning surfaces require a separate volumetric backend
and reference validation. Do not describe the entire issue as complete when only
the first milestone ships. T-junctions, near-coincident points, cracks, small
components, and inverted shells are follow-up scope, not requirements for #41.

## Decisions for implementation

1. Preserve analysis geometry before render processing. Keep imported point and
   face identities, positions, triangle connectivity, group membership, and a
   mapping from generated triangles to source faces. Render vertex IDs are not
   source point IDs. Store analysis coordinates as doubles, while documenting
   that promoting an imported float does not recover source-file precision.
2. Keep original topology separate from a derived, exact-position-welded topology.
   Indexed input defaults to original topology. STL defaults to exact-position
   topology within a source file because STL has no shared point-index topology.
   Offer original-index/exact-position analysis modes where applicable. Never weld
   separate source files implicitly; explicitly selected combined analysis can do
   so. Every result records its scope and topology mode.
3. Duplicate-point and duplicate-triangle definitions always use imported source
   records, independently of topology mode. STL index-based duplicate-triangle
   detection is unavailable; geometric duplicates can still be reported. STL
   repeated corner coordinates are informational, not automatically defects.
   Existing importer ABI v1 exposes the plugin's vertex table, not necessarily
   original file identities: label this provenance accurately without changing
   the ABI in this issue.
4. Source identity checks use source coordinates/indices. Geometric checks on a
   analysis use source world transforms, excluding the analysis display offset.
   Partition shared source points by transformed part instance where needed so
   independently transformed parts never accidentally share one analysis position.
5. Implement topology and quality checks with structs and deterministic free
   functions. Use a narrow free-function adapter for an intersection backend.
   Prototype CGAL's triangle-soup intersection checker first; select the production
   backend after correctness, cancellation, build-cost, and license review.
6. Keep detector settings owned by `UiState` through analysis settings; modify
   them via `ui_operations`, validate at operation/load boundaries, and persist
   them in `.woby`. Workers, timings, spatial indexes, and GPU resources remain in
   runtime/adaptor code. Rendering only consumes prepared state/results.
7. Preserve existing CLI diagnostic-field meanings. Add explicit versioned detector
   results rather than silently redefining `duplicateTriangles` or
   `degenerateTriangles`. Legacy geometry-duplicate and near-collapse counts remain
   available during migration. Separate non-manifold and winding display controls,
   migrating the existing combined visibility value to both.

## Detector contract

Thresholds below are proposed defaults from the issue's first parameter column.
The alternative column becomes test cases or saved user settings, not a second
implicit default. Define strict/inclusive comparisons in tests and documentation.
There is no global detector tolerance and no reuse of the distance-heatmap tolerance.

| Detector | Algorithm and result | Parameters and boundary behavior |
| --- | --- | --- |
| `duplicate_points` | Hash exact finite source-coordinate triples; report each occurrence after the first and its representative. Normalize signed zero for hashing. | No epsilon; unequal near-coincident points remain distinct. |
| `duplicate_tris` | Hash sorted source point-ID triples; report each occurrence after the first, regardless of winding. Scope IDs by source file. | No epsilon; coincident triangles with other IDs are not this finding. Include repeated degenerate faces in this independent check. |
| `degenerate_tris` | Union of collapsed/collinear, needle, and cap findings, with separate reason flags. Needles compare squared longest/shortest lengths; caps compare normalized dot products to an angle cosine. | `needle_threshold_ratio=1000`, `cap_min_angle_degrees=177.5`; use strict exceedance. Handle zero lengths before division. Count a face once in the union. Retain the legacy numerical-collapse metric separately. |
| `non_manifold_edges` | Undirected edge incidence greater than two, with all incident face references. | No geometric tolerance. One incident face is a boundary, not a non-manifold edge. |
| `non_manifold_vertices` | Vertex link graph must be one cycle for an interior vertex or one path for a boundary vertex. | Exclude endpoints of reported non-manifold edges, as the issue specifies. Unused vertices are a separate condition. |
| `inconsistently_oriented_tris` | Report faces incident to same-direction shared manifold edges; BFS flip constraints additionally identify orientation contradictions. | No angle/normal tolerance. Return conflict edges and both incident faces; do not claim a unique erroneous face or a repair direction. |
| `holes` | Trace simple boundary cycles and compare each loop's axis-aligned bounding-box diagonal with its component's diagonal. | `hole_size_ratio_tolerance=0.05`, inclusive `<=`. Use edge-connected components before fin splitting. Return branched/open boundaries separately; do not invent loops or silently divide by zero. |
| `self_intersections` | Spatial broad phase plus robust triangle-triangle predicates; return normalized unique face pairs and unique affected faces. | Exclude only valid shared vertices/edges under the declared topology. Include overlap beyond a shared feature and coplanar area overlap. Exact intersection initially has no user epsilon. |
| `fins` | Cut adjacency at non-manifold edges; classify boundary-bearing patches whose boundary is not one simple closed loop, then filter by area. | Require at least two split components. `max_area_ratio=1.0`, inclusive `<=`; denominator is the largest boundary-bearing candidate component, not whole-mesh area. Validate the exact candidate population against reference fixtures. |
| `non_partitioning_surfaces` | Build an intersection-resolved arrangement and classify aggregate patches using adjacent volumetric cells. | Exclude fin patches and patches containing detected holes. `max_area_ratio=1.0` and `hole_size_ratio_tolerance=0.05` feed those exclusions, not an independent error margin. Definition requires reference validation. |

Invalid coordinates/indices are validation failures, not clean detector results.
Exclude geometrically collapsed faces from surface adjacency/loop algorithms, retain
their source findings, and report the exclusion count. Keep nondegenerate duplicate
faces in incidence calculations; expose related findings so resulting edge defects
are explainable. Do not silently deduplicate or repair the mesh before detection.

The issue's GGM intersection tolerances (`1e-6`, `1e-9`) do not yet have a verified
meaning. Obtain its implementation or input/expected-output examples before claiming
parity. A proximity detector is a different feature from robust exact intersection.
The issue also references GMM sweeps without an implementation in this repository.

## PR 1: Preserve analysis input and source references

Touchpoints: `src/model_mesh.h`, `src/model_mesh.cpp`, `src/obj_mesh.cpp`,
`src/stl_mesh.cpp`, `src/importer_host.cpp`, `src/background_load.*`, and
`src/comparison_scene.*`. Proposed new files: `src/mesh_analysis_input.h/.cpp`.

- Add immutable analysis input owned with each loaded mesh, captured before
  `finalizeMesh`/`compactMesh`. Avoid copying it into every render-derived mesh.
- Preserve OBJ position indices independently of normal/UV splits, including
  unreferenced source points. Capture original polygon-to-triangle provenance during
  triangulation; diagnostics operate on generated triangles but identify the parent
  face. Do not promise original polygon IDs if the triangulator mapping is absent.
- For STL, preserve facet/corner references and explicitly describe derived topology.
  For plugins, capture the returned vertex/index table before optimization and before
  plugin memory is released. Keep ABI v1 working.
- Build analysis snapshots alongside analysis world geometry, with stable local
  triangle ordering and references scoped by source object and part instance.
- Track input capability/provenance so unsupported source-index checks report
  `unavailable`, rather than zero findings. Define whole-file versus selected-part
  source-point scope; whole-file inspection can include unused imported points.

Acceptance: fixtures cover OBJ UV/normal seams, distinct coincident source points,
triangulated polygons, STL facets, plugin buffers, independent part transforms,
and analysis offsets. Render optimization must not change analysis identities;
rendered geometry and current distance results must remain unchanged.

## PR 2: Shared topology and inexpensive detectors

Touchpoints: `src/mesh_comparison.h/.cpp`, `CMakeLists.txt`.
Proposed files: `src/mesh_diagnostics.h/.cpp`, `src/mesh_topology.h/.cpp`,
`tests/mesh_diagnostics_tests.cpp`.

- Build point canonicalization, edge-to-face incidence, vertex links, face adjacency,
  connected components, and boundary graphs once per input/topology configuration.
- Implement the first seven detector rows, including individual boundary findings.
- Introduce `DetectorKind`, `DetectorSettings`, typed point/edge/face/loop/patch
  findings, and per-detector completion/status data. Findings include source IDs,
  measurements, reason flags, and related elements; counts are derived consistently.
- Order output deterministically using source IDs and canonical element keys.
  Deduplicate union counts without losing per-reason membership.
- Accept stop tokens and check cancellation during preprocessing and result
  collection. Handle empty/invalid/unsupported input explicitly.

Acceptance: exact and near duplicates, every triangle-index permutation, repeated
degenerate faces, zero/collinear faces, needle/cap threshold boundaries, a valid
disk, closed tetrahedron, three faces on one edge, bow-tie vertices, winding
conflicts, orientation contradictions, multiple boundary cycles, branched boundaries,
and independent components. Test both topology modes and deterministic ordering.
Check translation/uniform-scale invariants for geometric quality; bounding-box hole
ratios are coordinate-frame dependent and must not be asserted rotation invariant.

## PR 3: Settings, persistence, scheduling, UI, and CLI

Touchpoints: `src/comparison_settings.h`, `src/ui_state.h`, `src/ui_operations.*`,
`src/scene_file.*`, `src/comparison_view.*`, `src/control_protocol.*`,
`src/control_scene.*`, `src/command_line.*`, `src/main.cpp`, `src/scene_pick.*`,
`src/comparison_report.*`, `src/scene_screenshot.cpp`, and `doc/ctl-commands.md`.

- Store per-analysis enabled detectors, thresholds, topology/scope, and independent
  overlay visibility. Validate finite values and sensible domains centrally:
  needle ratio >= 1, cap angle in [90, 180], nonnegative finite ratios. Ratios need
  not be capped at 1. Keep disabled distinct from completed-with-zero-findings.
- Write scene version 6 (or the next unused version at implementation time), retain
  v2-v5 loading, migrate legacy controls, and add normalized save/load mappings.
  Persist settings, not findings, caches, or transient finding selection.
- Extend analysis signatures to cover input revisions, membership, world transforms,
  analysis scope/mode, enabled detectors, thresholds, and algorithm revision. Keep
  analysis invalidation separate from overlay/color/display-offset changes and from
  distance-result invalidation. Reuse unchanged topology when only filters change.
- Update both interactive analysis workers and CLI-requested jobs in `main.cpp`
  to call the same analysis functions. Preserve the existing two-analysis limit.
  Bound snapshot/copy work and keep large preprocessing off the UI thread.
- Track queued/running/complete/disabled/unavailable/canceled/failed states. Show
  previous results as stale when appropriate; never publish stale findings as current.
- Add a detector list with counts, explanations, parameters, visibility, and a
  paged findings list. Selecting a finding highlights and frames its points, edges,
  faces, loop, or patch. Finding selection is transient. GPU overlays and bounds are
  built in runtime update code and honor analysis display offsets.
- Extend `analysis set` and `analysis results --json` with detector settings,
  schema version, status, topology/provenance, counts, and bounded/paginated element
  results. Keep existing fields and their meanings. Never emit an unbounded JSON
  payload for millions of findings.
- Update reports/screenshots to distinguish findings from candidate classifications
  and show relevant thresholds. Exports wait for requested visible analysis; report
  failed or unavailable required detectors explicitly instead of producing a clean
  looking partial result.

Acceptance: operation validation and dirty-state tests; v2-v5 migration and v6
roundtrips; legacy JSON compatibility; UI/CLI agreement; single-side inspection;
selection/highlight/source mapping; edits during a worker run; deletion/reload;
cancellation; screenshot waiting/error behavior; no recomputation on presentation
changes. Include a manual UI pass with points, edges, faces, and loop findings.

Milestone A: seven requested detector categories are usable end to end.

## PR 4: Robust self-intersections

Proposed adapter: `src/mesh_intersections.h/.cpp`. Also touch diagnostics, dependency
configuration, runtime status, overlays, and tests.

- Build a fixture-driven prototype of CGAL's triangle-soup checker. Account for its
  degenerate `(i,i)` output: route that to degeneracy findings, not intersection pairs.
- Check Windows Debug and existing CI platform builds, dependency/license suitability,
  memory overhead, and cancellation/shutdown behavior. Do not assume the library's
  single call can be canceled internally.
- If unsuitable, use Woby's BVH infrastructure with a Guigue-Devillers-style narrow
  phase backed by adaptive orientation predicates. Complete coplanar and shared-feature
  classification; merely copying a floating-point overlap test is insufficient.
- Keep backend choice out of logical scene settings. Record algorithm/version in
  computed reports. Preserve face provenance through backend reindexing.
- Add cancellation between bounded work units. Where a backend cannot provide
  acceptable interruption, replace or isolate that operation before shipping.
- Benchmark sparse and densely overlapping inputs. Intersection output itself can
  be quadratic; bound stored/displayed pairs, return an explicit truncated/incomplete
  status, and never advertise a partial affected-face count as exact.

Acceptance: crossing disjoint faces, valid shared edge/vertex, overlap beyond an
adjacent edge, different-index coincident faces, coplanar area overlap, T-contacts,
near misses, very small/large coordinates, zero-area input, and cancellation. Compare
against exhaustive pair checking on small generated meshes using an independently
validated predicate/reference corpus, not only another traversal of identical code.

Milestone B: eight requested categories, with robust intersection semantics.

## PR 5: Fin candidates

Touchpoints: shared topology, diagnostics settings/results, UI/report wording, fixtures.

- Reuse adjacency to form patches separated at non-manifold edges; retain provenance
  to original faces and distinguish physical from cut boundaries.
- Implement the issue's component-count, boundary-shape, and relative-area rules.
  Record candidate patch area, denominator, boundary classification, and ratio.
- Validate against a main surface with an attached fin, intentional sheets, multiple
  openings, closed components, and multiple small patches. Include cases where total
  mesh area and largest boundary-bearing candidate area give different answers.
- Obtain expected results for ambiguous boundary/candidate-population cases. Until
  those agree, label output as Woby fin candidates rather than GGM/GMM-equivalent.

Milestone C: nine categories. Fin output remains explicitly heuristic.

## PR 6: Non-partitioning surfaces

Begin with a bounded technical spike, followed by a production integration change.

- Obtain representative GMM sweep inputs and expected patch classifications. Resolve
  whether non-partitioning means the same adjacent arrangement cell, the same material
  label on both sides, or another reconstruction rule; these are not interchangeable.
- Prototype intersection resolution and volumetric cell extraction with a suitable
  exact-construction backend, such as libigl's CGAL-backed arrangement functions.
  Retain original-face provenance when intersections split triangles.
- Aggregate classified fragments into patches, then apply the specified fin/hole
  exclusions. Reuse the same settings and definitions as standalone detectors.
- Test a closed shell, cavity, disconnected internal sheet, sheet that partitions an
  enclosed region, intersecting shells, and patches containing fins/holes. Define
  handling of partial original faces and coincident oppositely oriented sheets.
- Measure build/runtime/memory cost and responsiveness. A winding-number approximation
  may be explored separately, but cannot substitute silently for exact cell semantics.
- If reference semantics or acceptable behavior cannot be established, report this
  detector as unavailable/experimental and keep this portion of #41 open.

Milestone D: all ten categories meet their documented contracts; reference parity is
claimed only where verified with expected-output fixtures.

## Validation and completion

Each behavioral PR includes unit tests without restructuring solely for testability.
Keep pure algorithms independent of ImGui, SDL, and bgfx. Add CLI and scene roundtrip
coverage where behavior crosses those boundaries. Run the full unit suite and build
Debug without warnings before each implementation PR is considered complete:

```powershell
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg
```

If an existing Woby instance blocks the build, terminate that instance and retry as
required by `AGENTS.md`. CI continues using Ninja + vcpkg. Place any implementation
worktrees under `D:\.worktree` and use `codex/` branch names.

Record analysis time, peak additional memory, and cancellation latency on the existing
sample plus synthetic sparse/dense meshes at increasing sizes. Establish numeric
budgets from a measured baseline during PR 1 and enforce them in later milestones;
do not invent performance guarantees in advance. Verify dependency packaging on all
existing CI platforms when adding a backend.

Complete delivery includes detector documentation, sample scenes/fixtures, parameter
units and threshold behavior, topology/provenance explanations, UI and CLI parity,
scene migration, and explicit limitations for heuristic or unavailable checks.

## Research references

- [CGAL shape predicates](https://doc.cgal.org/latest/Polygon_mesh_processing/group__PMP__predicates__grp.html)
- [CGAL triangle-soup intersections](https://doc.cgal.org/latest/Polygon_mesh_processing/group__PMP__intersection__grp.html)
- [Guigue-Devillers triangle overlap paper](https://www.tandfonline.com/doi/abs/10.1080/10867651.2003.10487580)
- [Shewchuk adaptive predicates](https://www.cs.cmu.edu/~quake/robust.html)
- [libigl mesh arrangements](https://libigl.github.io/tutorial/#boolean-operations-on-meshes)
- [libigl cell extraction](https://raw.githubusercontent.com/libigl/libigl/main/include/igl/copyleft/cgal/extract_cells.h)
- [CGAL package licenses](https://doc.cgal.org/latest/Manual/packages.html)
- [CGAL licensing options](https://www.cgal.org/license.html)
