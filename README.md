# woby

woby is a desktop application for viewing and analyzing 3D geometry.

## Features

- **Supports obj file format:** Triangular meshes, polygon meshes, point clouds and OBJ freeform geometry including Bézier and B-spline/NURBS curves and surfaces, with trim boundaries and holes.
- **Importers:** Write your [importers](doc/importers.md) to support any file format.
- **VTK tetrahedral meshes:** Load [ASCII unstructured grids](doc/vtk-meshes.md) with 4-node and 10-node tetrahedra. Importers can also return these cells. No VTK library is required.
- **AI support:** Tell your AI where to find woby executable, it will figure out the rest. CLI based http server that can do anything and everything in one or more woby instances.
- **Surface comparison:** Create named analyses with A/B inputs, overlays, and distance heatmaps. Set tolerances and color ranges. Results include maximum, mean, P95, and area statistics. Distances are approximate and unsigned. Measurements use the model coordinate units.
- **Mesh diagnostics:** Find boundary edges, winding edges, non-manifold edges and vertices, duplicates, degenerate triangles, holes, fin candidates, and self-intersections. Select findings to show them in the viewport.
- **Surface mesh quality:** Examine triangle size, shape quality, and local size changes. Results include histograms, percentiles, and comparisons with specified size limits.
- **UV inspection:** Examine an existing 2D UV layout or a 3D surface grid. Measure distortion, stretch, orientation, and overlaps. Select linked points in the source and analysis. Woby does not create UV coordinates.
- **Surface annotations:** Draw lines and rectangles on surfaces. Change their shape or add comments. Save annotations with the scene.
- **Saved states:** Save named Views and `.woby` scenes. Use Undo/Redo for scene changes, object additions and removals, and analysis settings.
- **Recent files:** Use File > Recent scenes or File > Recent models to open a file again. Each list keeps up to 20 files. Right-click an entry to remove it, or use File > Clear recent history to clear both lists.
- **Windows file associations:** In Settings, select `.woby` scenes or `.obj` models to add this copy to Open with. Use Windows defaults to select the default app. Registration applies to the current user and does not require administrator rights. If you move the portable folder, select the file types again to register the new copy. Clear the checkboxes before deleting a registered copy.
- **Export and automation:** Export PNG images at a specified resolution. Include analysis legends and labels if necessary. Export diagnostic results as JSON. Use the [CLI and local API](doc/ctl-commands.md) to control an active viewer.

Recent file history is saved for each user. On Windows, the file is `%LOCALAPPDATA%\woby\recents.json`. On macOS, it is `~/Library/Application Support/woby/woby/recents.json`. On Linux, it is `$XDG_STATE_HOME/woby/recents.json`, with `~/.local/state/woby/recents.json` as the default. History is separate from saved scenes and Undo/Redo. Headless sessions do not update it.

Use Ctrl-click to select multiple rows. Use Shift-click to select a range of
expanded rows. Right-click keeps the selection and adds the clicked row if
necessary. The context menu shows how many items an operation affects. Use
**Select all in this list**, or press Ctrl+A with the pointer over the list.

Bulk menus are available for models, analyses, annotations and cross sections,
analysis inputs, detector checks, model file removal, and saved Views. Each scene
edit uses one Undo step. File removal affects the scene; it does not delete files
on disk. In Views, Ctrl-click and Shift-click select rows without restoring a
view. A normal click restores the view.

## Requirements

| GPU and system | Driver version | Source information |
| --- | --- | --- |
| NVIDIA on Windows | Vulkan developer beta **595.92** | This release added device-address commands. Earlier releases added descriptor heaps and untyped pointers. Refer to the [NVIDIA release history](https://developer.nvidia.com/vulkan-driver). |
| NVIDIA on Linux | Vulkan developer beta **595.44.03** | This release added the same feature on Linux. Refer to the [NVIDIA release history](https://developer.nvidia.com/vulkan-driver). |
| AMD on Windows | Adrenalin **26.6.1**, with applicable RDNA 3 or later hardware | This is the first listed regular release with descriptor heaps and device-address commands. Preview **25.30.17.02** added descriptor heaps only. Refer to the [AMD Vulkan release history](https://www.amd.com/en/resources/support-articles/release-notes/rn-rad-win-vulkan.html). |
| AMD on Linux | **Mesa RADV 26.2.0** or later | This release enables descriptor heaps by default. Refer to the [Mesa 26.2 release notes](https://docs.mesa3d.org/relnotes/26.2.0.html). |

The x64 CPU minimum is AVX2. The operating system must enable AVX register state.
The Mac hardware minimum is Apple M1 or newer. Mac builds use ARM NEON.

Every x64 build includes AVX-512 coordinate rebasing. Woby selects it automatically
when the CPU and operating system support the required instructions and register
state. Other x64 systems use AVX2. No build option is needed.

| GPU and system | Minimum feature generation | Examples |
| --- | --- | --- |
| NVIDIA on Windows or Linux | Turing | GeForce GTX 1630, GTX 1650, GTX 1660, and RTX 2060. Later RTX generations also have the necessary architecture. GTX 10-series/Pascal does not have the necessary features. |
| AMD on Windows | RDNA 3 | Radeon RX 7600. Renderer reports also include Radeon 700M integrated graphics. |
| AMD on Linux with RADV | RDNA 2 | The AMD GPU in the Steam Deck. Radeon RX 6400 and RX 6600 are desktop candidates from this generation. |

## Screenshots

**Large models**

The Bear Trap ground model contains **75,771,986 triangles** and **38,414,391 vertices** in a 7.11 GB OBJ file. The lower-left status area shows the geometry counts and live FPS.

This capture shows **54.2 FPS** with solid rendering at **3840 × 2071** and 4× MSAA. It uses woby 0.27.0, a Release build, on Windows with an NVIDIA GeForce RTX 3070 Laptop GPU and an AMD Ryzen 7 5800H.

[![Bear Trap ground model with geometry counts and live FPS](doc/screenshots/beartrap-fps.png)](doc/screenshots/beartrap-fps.png)

**Surface comparison**

Compare an original surface with a repaired surface. The distance heatmap shows the changed areas. The Properties pane shows the tolerance, color range, and distance statistics.

[![Surface comparison with a distance heatmap and statistics](doc/screenshots/surface-comparison.png)](doc/screenshots/surface-comparison.png)

**Mesh diagnostics**

Find topology errors and select a finding to highlight the affected geometry. This sample shows a selected non-manifold edge, detector counts, and controls to move between findings.

[![Mesh diagnostics with a highlighted finding and detector counts](doc/screenshots/mesh-diagnostics.png)](doc/screenshots/mesh-diagnostics.png)

**Surface mesh quality**

Compare gradual and abrupt changes in triangle size. The selected result shows local size-jump statistics and a distribution histogram. Each result uses its own color range.

[![Mesh quality analysis with local size-jump colors, statistics, and a histogram](doc/screenshots/mesh-quality.png)](doc/screenshots/mesh-quality.png)

**Freeform geometry and UV inspection**

Inspect a trimmed Bézier surface with a circular hole beside its 2D UV layout. Cyan lines show constant U values; orange lines show constant V values.

[![Trimmed freeform surface and UV layout with U and V grid lines](doc/screenshots/uv-inspection.png)](doc/screenshots/uv-inspection.png)

**Surface annotations and saved views**

Add lines, review areas, and comments to a surface. Select an annotation to edit its shape and appearance. Save a named view to return to the same scene settings and camera position.

[![Surface annotations with edit handles, a comment, and a saved view](doc/screenshots/surface-annotations.png)](doc/screenshots/surface-annotations.png)

## Development tests

Build the Debug configuration, then run the CPU, slow, and graphics suites separately:

```powershell
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg-slow
ctest --preset vs2026-vcpkg-graphics
```

The presets use four CTest jobs. CPU tests run in source-file groups. Slow cases run separately and are excluded from their file groups. Graphics tests share a resource lock. Enable `WOBY_TEST_HEADLESS` at configuration time to include the headless rendering tests.

Use `ctest --preset vs2026-vcpkg -R scene_history` to run a file group. To run one case, call `build/vs2026-vcpkg/bin/Debug/woby_tests.exe --test-case="case name"`. Individual case names and source locations remain in failure output. CI uses Ninja and the same three suite filters.
