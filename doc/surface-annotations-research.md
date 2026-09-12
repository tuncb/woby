# Surface annotation research and implementation proposal

Research date: 2026-09-12. Repository reviewed at `285b292`.

The recommended feature is an Annotation scene item with two initial shapes: a surface line and a rectangular surface range. The user confirmed that rectangles should be projected onto the model so that they follow its surface. This is a research proposal; the feature has not been implemented or benchmarked.

An annotation starts as a shape drawn in the viewport, becomes attached to the selected mesh surface, and subsequently follows the model's transforms. A rectangle is rectangular in its creation projection; its edges can bend in 3D. Orbiting the camera must never change which surface is marked. This feature creates review metadata without modifying the imported mesh.

## Relevant precedents

| Application | Documented behavior | Implication for woby |
| --- | --- | --- |
| Blender | Annotation tools include lines and polygons. Surface placement draws onto objects, with an option to restrict projection to selected objects. Surface placement falls back to a cursor plane when no surface is hit. | Adopt explicit surface placement and target restriction. For woby, reject unsupported placement instead of creating a floating annotation. [Blender manual](https://docs.blender.org/manual/en/5.2/interface/annotate_tool.html) |
| CloudCompare | Trace Polyline picks points on visible meshes/clouds, can oversample segments using the current viewport to improve surface fit, and exports the result into its database tree. | Closest interaction precedent for drawing a line and converting it into persistent surface geometry. [Trace Polyline](https://cloudcompare.org/doc/wiki/index.php/Trace_polyline) |
| 3D Slicer | Markups have editable control points, surface snapping, model-constrained projected curves, and a separate shortest-path-on-surface mode. Its ROI is a box, distinct from its plane and curve tools. | Separate placement, editable controls, and geometry semantics. A projected line and a shortest surface path should be distinct tools if both are eventually offered. [Markups documentation](https://slicer.readthedocs.io/en/latest/user_guide/modules/markups.html) |
| Autodesk Navisworks | Markups are associated with viewpoints; creating one automatically saves the viewpoint. | Useful review precedent, but viewpoint attachment alone does not satisfy the requested persistent surface range. [Markup Tools Panel](https://help.autodesk.com/cloudhelp/2026/ENU/Navisworks/files/GUID-0E0EEEC6-CF7F-4458-8B5B-E18E1C954C8D.htm) |

These are documented comparisons, not hands-on tests of those applications. The Blender source is the versioned 5.2 manual; it is not a claim about the user's installed Blender version.

## Proposed interaction

1. Select a model part and choose **Add annotation > Surface line / Surface rectangle**, from the Objects context menu or a viewport toolbar.
2. Drag between two endpoints or opposite rectangle corners. Show the projected result while dragging, including invalid portions. The first version targets one mesh part; selecting a file can enter placement mode and let the first hit choose its part.
3. Release to commit a valid shape. Escape cancels. Require a continuous supported line or rectangle boundary; do not silently bridge a hole or jump to another surface layer. An invalid preview remains uncommitted and explains the problem.
4. Add a named Annotation item to the Objects pane. Properties show its target, shape, name, optional note, color, width, visibility, and lock state. Select it to edit endpoints or rectangle handles. Each completed drag is one undo action.

During a drawing drag, freeze the creation camera and capture the gesture before camera orbit/selection receives it. Release restores ordinary navigation. Editing a rectangle uses its stored projection frame, with handles projected into the current view; optionally offer **Return to drawing view** for easier editing. Changing the current camera never reprojects committed geometry.

By default, annotations hide with their source and are occluded by opaque geometry. Source deletion leaves a named missing-target item so its context is recoverable; undoing deletion restores attachment when the geometry matches. A locked annotation remains selectable but cannot be reshaped.

## What already exists in woby

Paths below are repository-relative implementation locations.

| Area | Existing implementation | Required extension |
| --- | --- | --- |
| Logical state and identity | `src/ui_state.h` has `UiState`, model groups, and separate `UiComparison` objects. `src/scene_objects.h` enumerates folder/file/group/comparison identities. | Add `UiAnnotation`, an annotations collection, and `SceneObjectKind::annotation`; extend ID allocation, enumeration, lookup, selection, and bounds. |
| Surface picking | `src/scene_pick.cpp:127` computes ray/triangle intersection parameters; `pickSceneObject` at line 297 returns only an object ID. | Introduce a surface-hit query returning the source part, triangle, barycentric coordinates, and hit position. Preserve current object-selection behavior. |
| Input and tree | `src/main.cpp` routes pointer gestures and draws the model tree and separate analysis items. | Route annotation gestures before orbit/selection; add annotation rows, creation commands, and inspector dispatch. |
| Transforms | `ScenePickPart::model` already contains the composed transform for each drawable part. | Resolve annotation vertices in the source's local coordinates, then apply that same composed transform. |
| Rendering | `src/scene_renderer.cpp:235` submits helper lines with `BGFX_STATE_DEPTH_TEST_ALWAYS`. | Add an annotation pass with depth testing and controlled bias. Existing helper rendering would show markings through the model. |
| Persistence | `src/ui_state.cpp:732` maps state to `SceneDocument`; comparison references map session IDs to file/group indices. `src/scene_file.cpp` currently writes version 7. | Add annotation records, source/anchor validation, load mapping, content equality, and a format revision. Do not serialize session IDs as durable references. |
| History | `src/scene_history.cpp` explicitly copies fields into snapshots and separately tracks identities. | Include annotations and missing target identities, plus geometry compatibility validation during restoration. Adding a vector to UiState alone is insufficient. |
| Views and export | `src/ui_view_operations.cpp` maps supported object kinds into saved views. `src/scene_screenshot.cpp` has a separate capture path. | Include annotation appearance/visibility in views and render annotations in scene PNGs. Exclude edit handles and unfinished previews from exports. |

All committed edits should go through `ui_operations` functions and advance the existing edit revision. Keep validation at operation/load boundaries. Rendering consumes resolved geometry and must not repair or mutate logical state.

## Data and attachment

Use structs and deterministic free functions, following the repository's architecture. Suggested types are `UiAnnotation`, `AnnotationDefinition`, `SurfaceAnchor`, and `AnnotationRuntime`.

`UiAnnotation` owns its identity, target reference, name/note, shape definition, display settings, and committed surface anchors. A `SurfaceAnchor` identifies a triangle within the source part and three barycentric weights. The local point is the weighted combination of that triangle's vertices. This representation supports points inside faces rather than snapping everything to existing vertices. CGAL documents this same face-plus-barycentric representation and ray-based location queries. [CGAL surface locations](https://doc.cgal.org/latest/Surface_mesh_shortest_path/index.html)

Persist the target's file/group record reference, readable source description, and a deterministic geometry fingerprint over imported positions and ordered triangle indices. Include importer context. Geometry changes or triangle reordering must produce a visible **Needs reattachment** state; an in-range triangle index alone is insufficient. Preserve the stored annotation, but do not silently attach it to unrelated geometry. Validate on Open and on history restoration that reloads a source. A future explicit repair operation can project onto a replacement mesh with a bounded tolerance.

Store the creation projector in source-local coordinates and the line endpoints or rectangle extents in its normalized drawing coordinates. This defines future reshaping independently of window size and current camera. Persist the committed face-local boundary segments as well, so reopening does not depend on rerunning the original visibility calculation. Define ownership of these values explicitly: edits regenerate definition and resolved anchors atomically; invalid loaded combinations are rejected or marked unresolved.

The runtime owns pointer capture, GPU buffers, acceleration structures, preview caches, and timing. A draft may hold prospective logical values, but must remain outside saved content/history until committed. The renderer may expand prepared segments for pixel width without changing their surface definition.

## Projecting the geometry

A straight segment between two picked 3D endpoints can pass inside a curved model. Likewise, connecting four picked corners with four chords does not produce a surface rectangle. Project the complete boundary.

For a fast preview, sample the 2D line or all four rectangle edges and cast rays against the target mesh. Adapt sampling around curvature and depth changes. Reuse the existing ray construction and triangle math, but provide separate surface semantics: annotation placement must not inherit x-ray edge selection or transparent draw-order behavior. Use the nearest target surface and reject placement hidden by another opaque object; identify a transparent selected target clearly in the preview.

For committed geometry, sampling alone is not a strict surface guarantee: adjacent samples may lie on different faces and their joining chord can cut across the mesh. Use triangle-clipped projected segments, or an equivalent triangle-walking construction, so every committed segment belongs to a triangle. In perspective, each screen segment defines a plane through the projection origin; intersect candidate triangles with that plane and restrict the intersections to the segment's projection interval. In orthographic projection, use the corresponding parallel-ray plane.

Resolve the frontmost valid surface along that interval. Join only continuous segments across valid mesh adjacency; disallow jumps between disconnected shells or across occlusion boundaries, even within one part. At folds, holes, non-manifold junctions, or ambiguous coincident faces, split the preview and reject unsupported completion for the initial release. OBJ/STL can contain duplicated vertices at seams, so connectivity construction must account for equivalent positions while avoiding accidental welding of nearby surfaces.

Use a geometry-local triangle bounding-volume hierarchy for repeated ray and clipping queries. Current object picking scans triangles after a bounds test; multiplying that cost by hundreds of samples would be a concern on large models. `src/mesh_comparison.cpp` already has an internal distance tree, but it is not a reusable ray-picking API and cannot be assumed to preserve the required face provenance. A shared geometry utility can be considered after profiling; no new geometry dependency is necessary for the first implementation.

## Rectangular ranges and rendering

Ship the persistent outline first. It signifies the region without implying a certified area measurement. A later translucent fill should consist of source triangles clipped to the stored rectangular projection and restricted to the intended visible surface sheet. Four corners do not describe an arbitrary curved interior. A flat quad would float or intersect the model; unrestricted projection can paint its rear wall as well.

Decal projection is an alternative rendering technique worth prototyping for fills. Three.js exposes a mesh/position/orientation/size decal API and explicitly notes corner distortion. This supports the feasibility of projection, not a recommendation to introduce Three.js into woby's C++/bgfx renderer. [DecalGeometry documentation](https://threejs.org/docs/pages/DecalGeometry.html)

Render committed outlines after surfaces with depth testing, no depth write, and a small controlled depth bias or geometric offset to avoid z-fighting. Use screen-expanded triangles if adjustable pixel width is needed consistently across graphics backends. Share the geometry/render path with PNG capture. Transparency, extreme zoom, tiny/large coordinate scales, and near-plane clipping require visual verification.

Measurements can follow separately. Projected width/height, straight endpoint distance, traced surface length, and surface area are different quantities. In particular, rectangle width times height is not curved surface area, and a projected line is not necessarily a shortest path. Retain woby's raw-coordinate convention if measurements are introduced.

## Delivery and verification

Implement in three reviewable steps: first exact surface picking and attachment utilities; then both drawing tools with scene state, tree, inspector, persistence, history, and rendering; finally robustness/performance/export verification before release. Optional fills, multi-part ranges, shortest-path lines, automatic reattachment, and annotation placement on analysis-result copies are later extensions. Analysis meshes are derived/recomputed, so attaching there would require an explicit source provenance mapping.

Add unit tests for hit coordinates; barycentric reconstruction; flat and curved boundaries; continuity across triangle edges and duplicated seams; gaps/folds/overlapping shells; target locking and occlusion; parent transforms; deterministic reconstruction; degenerate/nonfinite input; create/edit/cancel/undo/redo; old scene compatibility; save/load; missing or changed sources; saved views; and geometry replacement through history. Test that camera movement does not alter committed anchors or dirty state.

Save/load fixtures must use a unique temporary root per test, absolute related paths, and guaranteed cleanup, including failure paths. Test intentional cross-drive rejection separately. Run the Debug build and unit tests using `vs2026-vcpkg`, with no compiler warnings, as required by AGENTS.md. Visually inspect annotation occlusion, z-fighting, handle behavior, DPI scaling, and scene PNG exports. Benchmark interactive previews on representative large models before choosing a sample budget or latency target.

No application code was changed for this research. Build, unit tests, interactive rendering checks, and performance measurements have not been run.
