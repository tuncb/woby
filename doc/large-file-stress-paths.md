# Woby large file stress test paths

This catalog maps the paths through Woby 0.26.0 that can become expensive when a file, scene, or analysis grows. The primary corpus is `D:\temp\obj_tests`: polygon meshes, explicit and implicit point clouds, and native trimmed NURBS. [The measurement report](large-file-stress-results.md) distinguishes completed measurements, rejected inputs, crashes, resource limits, and remaining gaps. These are different outcomes; a failed import is never a rendering result.

The source baseline is `7f23b97560877266ca75fbc00c38a03e9576a656`. Application behavior is unchanged. The opt-in [runner](../tests/stress/run.py) uses the real viewer's local RPC API, background workers, production renderer, and scene operations.

## Opening and preparing data

| Path | What grows or blocks | How to exercise it | Evidence to retain |
| --- | --- | --- | --- |
| Process startup with an empty scene | SDL, Vulkan device, shaders, first graphics frame | Fresh process for each case | Startup events and first slow frames |
| Startup file argument | Import before instance readiness | Launch with `--file` | Launch to ready, CPU import, first capture |
| Add a file to an existing viewer | OBJ parser, triangulation, source identity, normals, bounds, GPU finalization | `model.add` | Wall latency, per-file outcome, CPU load log, memory |
| Recursive folder import, flat and tree | Discovery, small-file prefetch, per-file work, node count | `folder.add`, with and without `tree` | Batch totals, individual failures, tree/count checks |
| Add the same path again | Path lookup and repeated scene instances | Repeated `model.add` | Actual added/skipped counts and latency; do not assume deduplication |
| Polygon OBJ with normals and UVs | Corner interning and seam splits, material/group count | Bus map, San Miguel, powerplant | Source bytes versus render vertices/triangles/groups |
| Large mesh with few groups | Parsing, vertex/index storage, upload size | Bennu and BearTrap | CPU/GPU preparation, memory amplification |
| Vertex-only point cloud | Implicit point generation, source coordinate retention | SCARED-C, orchard | Point count, source bounds and origin, GPU limits |
| Explicit `p` cloud | Point references, duplicate/reused references, index arrays | testpoints and Semantic3D ladder | Point and vertex counts, memory versus size |
| Geographic coordinates | Double coordinates, localization and preserved origin | Orchard and geographically referenced meshes | `coordinateOrigin`, bounds, precision validation |
| Native rational and nonrational surfaces | Fallback freeform parser, control grids, knot spans | BeTSSi and keycap ladder | CPU load time, limits and errors |
| Trims, holes and curves | Trim evaluation, triangulation, tessellation | Camera and tubes ladder | CPU load, triangle/line counts, visual capture |
| Native geometry limits | Control/parameter/tessellation limits | 1,000 and 10,000-copy variants | Exact rejection stage; no extrapolated FPS |
| GPU buffer limits and memory pressure | Single-buffer size, staging copies, VRAM residency | 100M/full Semantic3D, orchard, BearTrap | Error, peak private/RSS, sampled device memory |
| Annotation preparation after import | Derived spatial blocks and fingerprints | Wait for `status.annotationReady` separately | Post-import wait, readiness/error, memory |
| Scene open into empty and populated viewers | Scene parsing, model reload, transactional replacement, old/new residency | `scene.open` both ways | Open time, peak memory, counts and capture |
| Mixed success and failure | Batch bookkeeping and recovery | Folder with valid and rejected inputs | Per-file outcomes and surviving scene |
| Cancel a pending import | Cooperative cancellation, worker cleanup, existing-scene preservation | Native Cancel during the 500-copy keycap triangulation | Canceled outcome, zero additions, surviving scene and post-cancel capture |

Entry adapters include File menus, drag-and-drop, CLI arguments, and RPC. They share loading operations but have different dialog/event overhead. An RPC result measures the shared loading path; it does not establish native dialog or drag-and-drop latency. Source: [background loading](../src/background_load.cpp), [OBJ construction](../src/obj_mesh.cpp), [freeform parsing](../src/obj_freeform.cpp), [tessellation](../src/freeform.cpp), [GPU resources](../src/scene_renderer.cpp), and [scene lifecycle](../src/scene_lifecycle.cpp).

## Drawing and navigating the scene

| Path | Stress axis | Controlled variants |
| --- | --- | --- |
| Opaque solid drawing | Triangles, pixels covered, draw calls | Whole model fitted, near/far camera, pane shown/hidden |
| Triangle edges | First-use line-index preparation, edge fill, retained buffers | Edges only and combined modes |
| Mesh vertex overlays | Point preparation, unique vertices, sprite fill | Vertices only; 1, 4 and 8-pixel points |
| Standalone point geometry | Point count, density, overdraw | Explicit/implicit clouds; point size ladder |
| Imported lines and tessellated curves | Segments, screen-space width, depth testing | Tubes/camera; line width and depth test |
| Combined solids, edges and vertices | Additional passes and GPU residency | All modes enabled |
| Transparency | Blending, depth state, overdraw | 35% opacity and opaque control |
| Source UV grid and gradients | UV availability and fragment shading | Grid density and U/V color modes |
| Visibility | Traversal and culling versus drawing | Visible/hidden whole file and hierarchy |
| Camera orbit, pan, dolly, roll and move | Cache invalidation and repeated frame construction | Repeatable RPC camera increments |
| Camera fitting | Visible bounds and hierarchy traversal | Whole scene, file, group, analysis, annotation |
| View helpers | Grids, axes, dimensions and labels | Helpers off/on |
| Object and Properties panes | Object count, UI construction, per-frame logical queries | Pane shown/hidden, many-group scenes |
| Hover and selection | CPU indexes, GPU picking/readback, cache reuse | Static/moving pointer; points and triangle selection |
| Screenshot export | Separate render dimensions, readback, PNG encoding, analysis waits | Default 1920 × 1800 capture; scene/analysis overlays |
| Window presentation | Display refresh, pacing, minimized/hidden window | Production desktop mode; separate from headless |

The frame stages are events, pending I/O, state update, ImGui build, scene state, view setup, hover pick, scene submission, helper submission, ImGui rendering, and graphics frame. GPU time overlaps CPU work; do not add them. The graphics-frame stage includes waiting and pacing. Headless mode renders requested captures but does not continuously submit the desktop scene, so its loop rate is not visualization FPS. Source: [main loop](../src/main.cpp), [renderer](../src/scene_renderer.cpp), [graphics](../src/graphics.cpp), [pacing](../src/frame_pacing.cpp), [hover](../src/hover_pick.cpp), and [marker picking](../src/marker_pick.cpp).

## Mesh diagnostics and surface quality

| Path | Stress axis | Variants and checks |
| --- | --- | --- |
| Analysis input snapshot and world mesh | Source copies, transforms, selected parts | File, group, multiple files; immutable worker snapshots |
| Boundary edges | Topology vertices/faces/edges | Original-index and exact-position modes |
| Non-manifold edges | High edge incidence and adjacency | Counts, bounded findings and overlays |
| Inconsistent winding | Orientation graph and conflicts | Both topology modes |
| Non-manifold vertices | Vertex links and incident faces | Threshold-free run, exclusions, navigation |
| Holes | Boundary components and ordered loops | Default and changed size-ratio threshold |
| Fins | Components, attachments and area ratios | Enable, change area threshold, retained findings |
| Duplicate source points | Exact-position hashes and source identity | Whole file including unused points; original transforms |
| Duplicate triangles | Canonical face records | Multiple groups and transformed instances |
| Degenerate triangles | Edge ratios and angles | Change needle and cap thresholds |
| Self-intersections | Broad phase, candidate pairs, exact predicates | Explicit run/cancel/retry; candidate and pair limits |
| Longest edge and equivalent size | Per-triangle geometry and distributions | Both heatmaps |
| Shape and size jump | Triangle shape and neighboring-face lookup | Both heatmaps |
| Findings pagination and focus | Large result collections and overlays | First/later pages; frame one finding |
| Analysis report export | Owned result snapshot, JSON streaming, disk I/O | Start, wait for completion, cancellation |
| Cached result retrieval | Serialization versus computation | Repeated `analysis.results` |
| Presentation-only edits | Cached measurements and regenerated display data | Offset, visibility, colors, Show flags |
| Geometry invalidation | Rebuilding dependent stages | Source transform, input enable/isolate/remove |
| Detector-specific invalidation | Reuse of unaffected stages | Topology mode and threshold edits |
| Concurrent analysis queue | Two workers, snapshot memory and stale work | Multiple analyses, cancel/delete/change source |

Source: [comparison snapshots](../src/comparison_scene.cpp), [analysis computation](../src/mesh_comparison.cpp), [topology](../src/mesh_topology.cpp), [duplicates](../src/mesh_duplicates.cpp), [degenerates](../src/mesh_degenerates.cpp), [intersections](../src/mesh_intersections.cpp), [surface quality](../src/surface_mesh_quality.cpp), and [result/export handling](../src/analysis_results.cpp). The standalone CPU benchmark reports stage costs separately from the viewer's snapshot and upload costs.

## Surface comparison

Test A→B and B→A distance sampling, BVH builds, sample storage, area-weighted statistics, display geometry, and upload. Start with identical inputs to check the near-zero distance result, then a translated independent copy for nonzero distances. This is sampled surface distance, not an exact Hausdorff distance.

Vary distance heatmap, A only, B only, overlay, and surface quality modes; change tolerance and color range; swap sides; enable/disable/isolate members; and query cached results. Scale both triangle count and group count. Include unequal sizes and partial overlap when preparing later comparison-specific fixtures. Source: [mesh comparison](../src/mesh_comparison.cpp) and [comparison scene](../src/comparison_scene.cpp).

## UV inspection and diagnostics

| Path | Stress axis | Variants |
| --- | --- | --- |
| UV inspection construction | Input mesh and layout generation | Supplied mesh UVs and generated native surface parameters |
| Surface and layout views | Display geometry and upload | 3D surface, overlapping 2D layout, separated patches |
| UV grid and U/V gradients | Display update and fragment shading | Density, color mode, range |
| Angle distortion | Per-triangle parameter derivatives | Metric selection and statistics |
| Area distortion | Signed log area ratio | Per-patch and absolute normalization |
| Orientation | Triangle winding and mixed patches | Uniform and mixed orientation |
| Anisotropy | Singular-value ratio | Threshold and range highlighting |
| Minimum stretch | Near-collapse behavior | Near-collapse threshold |
| UV overlap | Candidate generation and pair storage | Per-patch and selected-patches scope, truncation |
| Missing/collapsed UVs and degenerate surfaces | Classification and omitted layout parts | Preserve unavailable/invalid counts |
| Triangle pagination | Retained metric table serialization | First and later pages |
| Linked surface/layout probe | Identity mapping and display coordinates | Set/get/clear probe |
| Cache reuse and invalidation | Metric statistics versus full geometry | View, separation, normalization, source edits |

Source: [UV quality](../src/uv_quality.cpp), [overlap](../src/uv_overlap.cpp), [UV control](../src/control_uv.cpp), and [UV layout](../src/comparison_scene.cpp). Inputs without UVs are not equivalent to a successful UV workload. Overlap truncation is a bounded partial result, not an all-clear.

## Editing and scene lifetime

Exercise file/group transforms, scale, rotation, color, opacity, and visibility; large object inventory and scene-tree queries; saved-view create/apply/update; undo/redo; save; transactional reopen; clear and reopen; remove and restore; and repeated import/mode-toggle/remove cycles. Record both transient peaks and empty-scene memory after cleanup. Allocator retention and history retention are not automatically leaks.

For annotations, measure post-load preparation separately from line/rectangle projection, move, reshape, style edits, list/get, source transforms, rendering, persistence, deletion, undo, and redo. Use successful projected geometry to benchmark edits; projection rejection measures a different path. Source: [UI operations](../src/ui_operations.cpp), [history](../src/scene_history.cpp), [scene persistence](../src/scene_file.cpp), [annotation preparation](../src/annotation_preparation.cpp), and [surface annotations](../src/surface_annotation.cpp).

## Scope boundaries

The corpus does not supply every possible geometry pathology or importer format. Native file-dialog interaction, physical input latency, a deliberate OS cache flush, every GPU/driver, plugin importers, missing-network paths, and power-loss behavior need separate experiments. Existing unit/integration tests establish correctness for many of these paths, but are not substituted for large-file performance measurements. The results report lists the actual coverage and limitations.
