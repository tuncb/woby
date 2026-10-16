# woby changelog

Notable changes by release tag. Versions with no notable changes are omitted.

## v0.30.0

- Added VTK ASCII unstructured grids with linear and quadratic tetrahedra, including importer support.
- Added OBJ vertex colors and manual color overrides saved in scenes.
- Added actions to align selected object origins and arrange selected objects in a row.
- Added length and area measurements to annotation properties.
- Kept large model loads responsive with background preparation and streamed GPU uploads.
- Accelerated OBJ parsing, normal generation, annotation and point preparation, and spatial sorting.
- Reduced delays when selecting large models.
- Added AVX2 and Apple NEON acceleration. x64 systems now require AVX2; supported CPUs use AVX-512 for coordinate rebasing.

## v0.29.0

- Added recent scene and model file menus.
- Added Windows file association settings for `.woby` and `.obj` files.
- Added support for long paths and Unicode file names on Windows.
- Added cross-section annotations with Shift snapping and CLI controls.
- Added difference meshes, automatic closure for simple open surfaces, and OBJ export.
- Added adjustable comparison color limits, range presets, and tolerance handles.
- Reduced memory use in analyses and Undo/Redo by sharing mesh and annotation geometry.
- Reused UV measurements when display settings change.
- Reduced UV overlap search work and reported limits for partial results.
- Improved exact self-intersection check performance.

## v0.28.0

- Unified OBJ loading for meshes, lines, points, and freeform geometry.
- Added adaptive point rendering on Apple M1 and newer Macs.
- Kept vertices and point clouds opaque when surfaces are transparent.
- Blended transparent surfaces and triangle edges independently of draw order.
- Added automatic camera clipping and camera controls in Settings.
- Checked child volumes separately by default, with an option to combine them.
- Added specific error details when importer plugins fail to load.

## v0.27.0

- Added OBJ polylines, explicit points, and point clouds.
- Added Bézier and B-spline/NURBS curves and surfaces, including trim boundaries and holes.
- Extended importer ABI to version 4 for point and freeform geometry.
- Removed built-in STL support. Additional formats require importer plugins.
- Added UV stretch metrics, overlap checks, histograms, and linked point inspection.
- Separated mesh checks, mesh quality, and surface comparison into dedicated inspectors.
- Added controls to isolate analysis inputs.
- Added adaptive rendering and picking for large point sets.
- Added visible triangle edges and an X-ray display mode.
- Moved annotation projection and analysis preparation to background workers.
- Kept large imports responsive and improved recovery from memory allocation failures.
- Cached scene queries and metric display data, and improved diagnostic JSON export performance.

## v0.26.0

- Added UV patch isolation, linked selection, and parameter coloring.
- Added UV quality checks for angle distortion, area stretch, and orientation.

## v0.25.0

- Added 2D UV layouts and 3D surface grids with separate U/V controls.
- Added importer hierarchy and line groups with appearance controls.
- Upgraded importer ABI to version 3 and preserved original point identities.
- Added CLI controls for imported groups and UV views.

## v0.24.0

- Preserved large-coordinate precision with automatic working origins.
- Upgraded importer ABI to version 2 with double-precision positions, group colors, and visibility.
- Reported GPU requirements and detailed Vulkan startup failures.
- Added a master visibility control for diagnostic overlays.

## v0.23.0

- Replaced bgfx with NoGraphicsAPI for Vulkan and Metal rendering.
- Improved depth precision with reversed-Z rendering.
- Improved frame pacing on Windows.

## v0.22.0

- Improved OBJ loading performance and reduced geometry memory use.
- Generated vertex markers in shaders and added GPU picking.
- Prepared annotation data in background workers.
- Reused indexed vertices in comparisons to support larger analyses.
- Adapted camera clipping to scene bounds.

## v0.21.3

- Allowed annotations to bridge changes in surface depth.

## v0.21.2

- Reduced repeated memory allocation during rendering and picking.

## v0.21.0

- Added portable importer packages and preserved them during application updates.

## v0.20.5

- Added annotation renaming and duplication.
- Increased the importer buffer limit to 4 GiB.
- Improved self-intersection checks, annotation gestures, and surface picking performance.

## v0.20.3

- Saved diagnostic finding selections in named views.

## v0.20.2

- Added CLI controls to focus diagnostic findings.
- Added paginated diagnostic JSON export and self-intersection work limits.

## v0.20.1

- Allowed annotations to span multiple model parts.
- Saved scenes with model files on different Windows drives.

## v0.20.0

- Added headless rendering for CLI automation and image export.
- Improved annotation preview performance on dense meshes.

## v0.19.3

- Allowed annotation drawing on transparent surfaces.

## v0.19.0

- Added exact self-intersection checks and fin candidate detection.
- Added manual update, automatic update, cancel, and retry controls for each detector.
- Added a main menu for scene actions and settings.
- Added manual path dialogs when the Linux file chooser is unavailable.

## v0.18.1

- Allowed annotations to bridge holes in a surface.
- Improved annotation stroke precision.

## v0.18.0

- Added degenerate triangle, non-manifold vertex, and hole detectors.
- Added separate boundary, winding, and non-manifold edge controls.
- Improved model loading and mesh analysis performance.

## v0.17.0

- Added a file-row button to create analyses.
- Improved annotation projection performance.

## v0.16.0

- Improved OBJ import performance with RapidOBJ.
- Added exact duplicate point and triangle detection.
- Reused completed analysis stages when settings change.
- Added surface line and rectangle annotations with comments and CLI controls.
- Restarted the application after updates installed from Settings.

## v0.15.0

- Added navigation and camera framing for individual diagnostic edges.
- Renamed comparisons to analyses in the UI, CLI, and scene files.
- Added CLI camera placement, look-at, and object framing controls.

## v0.14.0

- Added CLI controls for saved views and camera presets, including isometric views.

## v0.13.0

- Added named saved views for camera and scene state.
- Added camera presets and selection framing.
- Displayed selected mesh dimensions and grid spacing.
- Added update checks and installation in Settings.

## v0.12.0

- Added surface mesh quality metrics, histograms, and size limits.
- Saved and restored the camera with each scene.
- Added inline comparison renaming and controls to enable or disable inputs.

## v0.11.0

- Added viewport selection of model parts and comparison results.
- Added Undo/Redo for scene edits, with CLI controls.

## v0.10.0

- Added mesh inspection without a second comparison input.
- Added a Settings dialog for UI scale.
- Added CLI self-updates with verified release packages.
- Added macOS app bundles and Windows console integration.

## v0.9.0

- Added a Properties inspector for single and multiple selections.
- Added comparison legends, distance statistics, and configurable PNG export.
- Added scene keyboard shortcuts and display presets.
- Moved importer management to the CLI.
- Restricted scene rendering and camera input to the viewport.

## v0.8.0

- Added multiple named comparisons with separate inputs, settings, and result positions.
- Compared files, folders, and mesh parts in groups A and B.
- Added large mesh comparisons with cancellation.
- Added CLI comparison controls and measurements.

## v0.7.0

- Added a local HTTP API and CLI control of viewer instances.
- Added native importer plugins and an OFF example.
- Added surface distance heatmaps, overlays, and mesh diagnostics.
- Added tree selection and comparison context menus.
- Added command queues, retry recovery, and status tracking for automation.

## v0.6.0

- Added folder tree import with shared visibility and transform controls.
- Saved folder hierarchy in scenes.

## v0.5.0

- Added scene screenshot export to PNG.

## v0.4.0

- Added ASCII and binary STL loading.
- Loaded models in the background with progress indicators.
- Added camera roll and saved Y/Z up-axis selection.
- Added file logging and optional performance logging.

## v0.3.0

- Added drag and drop for models, scenes, and folders.
- Added recursive folder loading and CLI scene loading.
- Added unsaved-scene prompts and scene-wide visibility controls.
- Added origin axes and a ground grid.
- Added macOS arm64 releases.

## v0.2.0

- Added `.woby` scene save and load.
- Added file dialogs, empty startup scenes, and file removal.
- Displayed vertex coordinates on hover.
- Added a resizable sidebar with scene and file sections.
- Added 4× MSAA.

## v0.1.0

- Loaded multiple OBJ files in one scene.
- Added orbit, pan, and zoom camera controls.
- Added solid, triangle edge, and vertex display modes.
- Added group colors, visibility, transforms, opacity, and vertex size controls.

## v0.0.2

- Renamed the executable to `woby`.

## v0.0.1

- Added the initial OBJ viewer.
- Added CLI model loading and an FPS counter.
