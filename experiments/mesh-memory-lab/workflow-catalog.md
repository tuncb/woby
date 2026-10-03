# Mesh lab workflow catalog

This catalog proposes 80 diagrams for understanding woby and comparing changes
between commits. Each diagram should expose its inputs, transformations,
intermediate data, decisions, and outputs. IDs 1–64 preserve the original list;
IDs 65–80 add workflows identified in the UV quality merge reviewed below.

All entries can be authored as `.meshflow` diagrams. The lab's existing live
inspectors cover the polygonal OBJ pipeline: source, parsing, attributes,
triangulation, corners, packing, CPU mesh, upload, and GPU buffers. Other live
inspectors require additional bindings or captured data. A production feature
listed here does not imply that its lab inspector is already implemented.

The current examples are in [workflows](workflows/). The
[file format](workflows/README.md) describes the supported nodes and bindings.

## Recommended starting set

Start with source-to-render vertex mapping (1), large-coordinate precision (6),
topology construction (9), surface-distance comparison (25), surface picking
(37), file-to-first-visible-frame (41), analysis cache invalidation (45), and
undo/redo restoration (52). These cover geometry, numerical correctness,
interaction, concurrency, and application state.

For the newly merged UV work, prioritize local stretch (65), signed area colors
(67), overlap detection (71), linked probes (74), and revision-checked triangle
pages (77). These have useful intermediate values and clear cases for comparing
two revisions.

## Import and mesh construction

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 1 | Source-to-render vertex mapping | Source position IDs → corner tuples → lookup → existing/new vertex → index buffer. Inspect why vertices split or remain shared. |
| 2 | Concave polygon triangulation | Polygon → projected contour → triangulation → triangles. Compare diagonals, winding, and output ordering. |
| 3 | Authored versus generated normals | Normal records → validation → authored/fallback branch → packed normals. |
| 4 | UV seam splitting | Shared positions + different UV references → separate render vertices. Compare vertex counts and connectivity. |
| 5 | Hard-edge splitting | Shared positions + different normals → split vertices → shaded result. |
| 6 | Large-coordinate precision | Original doubles → file origin → local doubles → GPU floats → scene placement. Inspect precision lost at each conversion. |
| 7 | Mixed OBJ primitives | OBJ records → faces / polylines / points → separate parts → corresponding render paths. |
| 8 | Importer plugin lifecycle | Format discovery → importer selection → plugin call → geometry validation → owned application data. |

## Topology and defect detection

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 9 | Topology construction | Source identities → vertices → edge uses → face adjacency → connected components. Compare original-index and exact-position modes. |
| 10 | Boundary and hole detection | Edge incidence → boundary edges → loops/open chains/branches → size filtering → findings. |
| 11 | Non-manifold detection | Edge incidence and vertex links → classification → offending edges/vertices → navigation geometry. |
| 12 | Winding consistency | Directed edge uses → neighboring face orientation → conflicts → highlighted faces. |
| 13 | Duplicate point detection | Source points → grouping keys → duplicate groups → original point references. |
| 14 | Duplicate triangle detection | Triangle references or coordinates → canonical keys → duplicate groups. Explain identity and geometric duplicates. |
| 15 | Degenerate triangle detection | Triangle coordinates → lengths/areas → configured criteria → classification → overlays. |
| 16 | Self-intersection detection | Spatial candidates → triangle-pair tests → contact exclusions → intersections → result publication. |

## Freeform geometry and UVs

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 17 | Bézier curve evaluation | Control points → parameter samples → basis weights → evaluated points → line segments. |
| 18 | B-spline/NURBS evaluation | Knots + degree + weights → active spans → basis tables → rational evaluation → geometry. |
| 19 | Surface tessellation | Control net → parameter grid → evaluated positions/normals → triangle connectivity. |
| 20 | Trimmed surface construction | Trim curves → sampled UV boundaries → constrained triangulation → retained regions → surface evaluation. |
| 21 | CPU/GPU freeform evaluation | Shared inputs → CPU and GPU evaluation → precision eligibility/fallback → cached geometry. |
| 22 | UV layout extraction | Triangle corners → supplied UVs → display coordinates → layout geometry. Preserve overlaps and tile offsets. |
| 23 | UV grid rendering | Interpolated UVs → independent U/V density → grid coverage → surface color. |
| 24 | UV quality analysis | Surface and UV triangles → angle/area/orientation/stretch measurements → validity and overlap checks → findings and colors. Use this as the overview for detailed workflows 65–80. |

## Analysis and surface comparison

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 25 | Surface-distance comparison | Two selected inputs → transformed analysis meshes → target acceleration structure → samples → distances → heatmap. |
| 26 | Comparison direction | A-to-B and B-to-A branches → separate samples → separate statistics. Show why results can differ. |
| 27 | Distance statistics | Samples + associated areas → maximum/mean/P95/area statistics → report values. |
| 28 | Mesh quality metrics | Triangle geometry → longest edge / equivalent size / shape / neighboring size jump → distributions. |
| 29 | Fin candidate detection | Topology → patches → physical and cut boundaries → boundary classification → area-ratio filtering. |
| 30 | Threshold filtering | Raw measurements → tolerance/range settings → selected findings → counts and overlays. Include which stages are reused or recomputed. |
| 31 | Analysis source assembly | Selected folders/files/parts → eligible triangular parts → deduplication → transformed source snapshot. |
| 32 | Finding navigation | Detector result → finding index → source references → bounds → camera framing and highlighting. |

## Rendering and interaction

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 33 | Complete frame rendering | Logical scene → visible draw items → transforms/resources → render passes → overlays → UI → presentation. |
| 34 | Solid, edge, and vertex modes | Shared geometry → enabled display branches → mode-specific resources and draw calls. |
| 35 | Hierarchical appearance | Folder/file/part settings → composed transforms and opacity → effective draw state. |
| 36 | Transparency and selection | Visibility/opacity → eligible draw and pick candidates → ordering → chosen object. |
| 37 | Surface picking | Pointer → camera ray → candidate geometry → intersections → nearest eligible hit → object or linked triangle selection. |
| 38 | Vertex marker picking | Visible markers → screen-space/sample coverage → candidate resolution → point identity → selection. |
| 39 | Camera projection and framing | Bounds + camera state → target/distance → view/projection matrices → screen coordinates. |
| 40 | PNG export | Export settings → result readiness → offscreen rendering → annotations → readback → image encoding. |

## Background work and performance

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 41 | File-to-first-visible-frame | File discovery → read/parse → mesh preparation → upload → first draw. Identify the critical path. |
| 42 | Recursive batch import | Folder traversal → supported files → queued loads → progress aggregation → scene insertion. |
| 43 | Cancellation | User request → cancellation token → worker checkpoints → discarded partial work → cleanup. |
| 44 | Stale-result rejection | Input revision → owned worker snapshot → completed result → revision check → accept/discard. |
| 45 | Analysis cache invalidation | Geometry/settings change → affected stages → retained caches → recomputation → publication. Track the actual dependency rules. |
| 46 | Incremental detector results | Shared preparation → independent detectors → staged results → counts and overlays becoming available. |
| 47 | Resource ownership and lifetime | Source storage → CPU arrays → upload memory → GPU buffers → readbacks → release points. |
| 48 | Allocation and reuse | Repeated operation → reusable storage lookup → allocation/reuse → peak memory and retained capacity. |

## Scene editing and automation

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 49 | Property editing | UI input → local edit → validation → operation → logical state → invalidated derived data → redraw. |
| 50 | Multiple-selection editing | Selected objects → mixed-value detection → edited field → per-object changes → resulting state. |
| 51 | Continuous edit history | Drag start → intermediate edits → drag end → one history entry. |
| 52 | Undo/redo restoration | History entry → source availability checks → optional reload → restored settings → scene publication. |
| 53 | Scene save/load round trip | Logical state → `.woby` mapping → relative paths → saved document → validation → reconstructed scene. |
| 54 | Saved views | Camera and view settings → named snapshot → selection → restored review state. |
| 55 | Surface annotations | Pointer hit → prepared geometry → surface location → line/rectangle construction → persisted annotation → rendering. |
| 56 | CLI automation request | Command → protocol validation → application queue → UI operation → completion/error response. |

## Development and workflow tooling

| ID | Workflow | Diagram and inspection focus |
| --- | --- | --- |
| 57 | Workflow file loading | Folder discovery → `.meshflow` parsing → validation → optional mesh capture → library publication. |
| 58 | Commit snapshot comparison | Revision folders → matched workflow identities → top/bottom panes → selected stages and captured results. Historical result capture is a proposed extension described below. |
| 59 | Build and asset staging | CMake configuration → dependencies → compilation → shaders/assets/workflow copies → runnable application. |
| 60 | Test execution | Changed subsystem → unit tests → GPU checks → rendered cases → results and artifacts. |
| 61 | Performance experiment | Fixture + configuration → repeated runs → timing/allocation captures → aggregation → comparison report. |
| 62 | Release packaging | Version metadata → build outputs → package assembly → manifest validation → release artifacts. |
| 63 | Application update | Update discovery → package verification → extraction → helper handoff → replacement → restart. |
| 64 | Renderer startup diagnostics | Device discovery → capability checks → device selection → resource initialization → success or diagnostic report. |

## Additions from the UV quality merge

Reviewed on 2026-10-03 at merge commit
`5acaf80e84c5b535dbc8fa5fc68d369d50d3803c`, which merged
`t3code/review-nurbs-parametrization`. The comparison used the first parent,
`002dd0e`, so the additions below describe what arrived on main in this merge.
The merged branch tip was `d6b7ab5`.

The merge extends UV metrics from angle, area, and orientation to include
anisotropy, minimum stretch, and overlap. It also changes signed-area coloring,
adds statistics and range highlighting, introduces linked barycentric probes,
and exposes detailed results through the CLI. It integrates those operations
with worker preparation, revision checks, and persisted settings.

Entries 65–80 are focused diagrams for those additions and changed behaviors.
They expand the overview entries above; they do not imply 16 separate new
production features. Measurements describe the supplied triangle mapping,
including tessellated freeform geometry, rather than exact CAD derivatives.
See the [UV command reference](../../doc/ctl-commands.md#patch-inspection-gradients-and-uv-quality).

| ID | Workflow | Diagram and inspection focus | Implementation references |
| --- | --- | --- | --- |
| 65 | Local UV stretch and anisotropy | 3D triangle → orthonormal tangent frame → surface-to-UV Jacobian → scaled singular-value calculation → minimum/maximum stretch → anisotropy. Inspect shear, uniform scaling, and nearly rank-one maps. | [Metrics](../../src/uv_quality.cpp), [regressions](../../tests/uv_quality_tests.cpp) |
| 66 | UV collapse and near-collapse classification | Surface/UV edge scales → relative area checks → missing/collapsed/degenerate exclusions → minimum-stretch threshold → near-collapse findings. Keep already collapsed triangles out of near-collapse counts. | [Metrics and findings](../../src/uv_quality.cpp) |
| 67 | Signed UV area heatmap | UV area / physical surface area → optional normalization → signed log2 → color encoding → shader → legend. Negative values show compression, positive values expansion, and ratio 1 is neutral. Compare the previous magnitude-only coloring. | [Metric encoding](../../src/uv_quality.cpp), [shader](../../shaders/native/woby.slang), [legend](../../src/comparison_legend.cpp) |
| 68 | Area and stretch normalization | Valid triangles grouped by imported patch → total UV / surface area → area normalization and square-root stretch normalization → reported units. Compare absolute and per-patch results; anisotropy remains unchanged by uniform scaling. | [Normalization](../../src/uv_quality.cpp), [controls](../../src/uv_quality_view.cpp) |
| 69 | UV statistics and dual histograms | Valid metric values → sorted face values → minimum/median/P95/maximum → 12 bins with face counts and physical surface areas. Inspect why equal face weights and area percentages answer different questions. | [Statistics](../../src/uv_quality.cpp), [histogram UI](../../src/uv_quality_view.cpp) |
| 70 | Threshold and histogram selection | Metric → greater-than / absolute-area-magnitude / lower-stretch threshold → optional selected range → highlighted faces and area → cyan overlay. Show range precedence, inclusive range settings, histogram bin boundaries, and invalid-value colors. | [Selection rules](../../src/uv_quality.cpp), [range controls](../../src/uv_quality_view.cpp) |
| 71 | UV overlap detection and domain scope | Valid UV triangles → patch/domain grouping → bounding boxes sorted along U → candidate pairs → V rejection → positive-area overlap → within-patch/cross-patch findings. Compare independent patches with an explicitly shared domain; display separation leaves the calculation unchanged. | [Overlap detector](../../src/uv_overlap.cpp), [settings](../../src/comparison_settings.h) |
| 72 | Robust UV contact classification | Orientation determinant → floating-point confidence check → exact rational fallback when needed → strict separating-axis tests → contact or overlap. Exclude shared edges/vertices; include containment and coincident triangles. | [Geometric predicates](../../src/uv_overlap.cpp), [contact fixtures](../../tests/uv_quality_tests.cpp) |
| 73 | Bounded and canceled overlap work | Candidate enumeration → cancellation checkpoints and candidate/pair limits → complete or explicitly truncated result → UI/report status. Inspect the 2,000,000-candidate and 10,000-pair limits and avoid interpreting partial results as proof of no overlaps. | [Limits](../../src/uv_quality.h), [worker checks](../../src/uv_overlap.cpp), [reporting](../../src/comparison_report.cpp) |
| 74 | Linked barycentric triangle probe | Solid source/analysis hit → part and triangle identity → barycentric weights → validated probe → matching triangle and point in both views. Finding rows and overlap-pair buttons can select centroids through the same probe state. | [Picking and overlays](../../src/scene_pick.cpp), [operations](../../src/ui_operations.cpp), [probe UI](../../src/uv_quality_view.cpp) |
| 75 | Probe coordinate reconstruction | Part transform + source triangle + barycentric weights → surface position and interpolated UV → layout/separation/analysis translation → display position. Track scene coordinate origin, existing OBJ V flip, and null positions for unavailable layout geometry. | [Source triangle](../../src/comparison_scene.cpp), [probe response](../../src/control_uv.cpp) |
| 76 | Probe validity and lifetime | Probe creation → current geometry/settings signature → input or linked-selection change → stale probe hidden/rejected → clear or reprobe. Show why probes are transient and do not dirty the document or enter scene/history snapshots. | [Probe state](../../src/ui_state.h), [validation](../../src/ui_operations.cpp), [stale responses](../../src/control_uv.cpp) |
| 77 | Revision-checked UV triangle pagination | Wait for current analysis → first page → result revision → subsequent bounded pages → revision mismatch/restart. Inspect zero-based offsets, one-based triangle IDs, limits of 1–100, all metric fields, and null unavailable/unchecked values. | [Page serialization](../../src/control_uv.cpp), [revision guard](../../src/main.cpp), [protocol](../../src/control_protocol.cpp) |
| 78 | UV diagnostics through headless automation | Create analysis → configure each of six diagnostics → await results → inspect pages/probes → render colors → save/reload → compare measurements and geometry. Use the same fixture to compare CLI, CPU results, and GPU presentation across commits. | [End-to-end regression](../../tests/ctl_uv_quality_smoke.py), [command reference](../../doc/ctl-commands.md#patch-inspection-gradients-and-uv-quality) |
| 79 | UV worker preparation and immutable publication | Owned input snapshot → world-space measurements → display mesh → overlap computation on a copied quality result → prepared buffers → publication of current results. Track cancellation, ownership, and signatures containing metric, normalization, overlap, threshold, and range settings. | [Preparation and signatures](../../src/comparison_scene.cpp), [result reuse](../../src/mesh_comparison.cpp) |
| 80 | UV settings validation and persistence | UI/CLI settings → validation/normalization → logical analysis settings → history/saved Views/scene serialization → reload → recomputation. Compare invalid command rejection with load normalization; preserve measurement controls while excluding transient probes. | [Settings boundaries](../../src/mesh_comparison.cpp), [scene mapping](../../src/scene_file.cpp), [regressions](../../tests/uv_grid_tests.cpp) |

## Captures for comparisons between commits

The current format stores diagram structure and optional embedded OBJ text.
The running lab rebuilds live results from that source using its current code.
A folder named after a commit therefore identifies a workflow snapshot, but it
does not by itself reproduce that commit's historical intermediate results.

For historical execution comparisons, a future capture format should record:

- Commit ID, capture version, build configuration, and relevant platform/backend.
- Input identity, fixture contents or checksums, and exact operation settings.
- Stable workflow node IDs and source part/triangle/vertex identities.
- Intermediate record counts, layouts, payload sizes, and selected sample values.
- Stage timings, repeated-run information, allocation counts, and peak memory.
- Branch decisions, cache hits, invalidation reasons, cancellation, and rejection status.
- Output checksums, diagnostics, validity flags, and explicitly partial results.
- For UV workflows, mapping convention, normalization/domain scope, statistics,
  overlap pairs, result revisions, and probe coordinates with their reference frames.

These are proposed capture fields. Adding them requires a format extension and
load/inspection support; unknown fields are rejected by the current `.meshflow`
reader. Use the same inputs and settings for both commits, preserve logical node
IDs when stages retain their meaning, and distinguish structural diagram changes
from measured behavior changes.
