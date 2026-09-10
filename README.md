# woby

woby is a desktop OBJ scene viewer for loading, inspecting, arranging, and saving Wavefront OBJ models.

## Features

- Load Wavefront OBJ and STL model files from the UI, command line, drag and drop, or recursive folder import.
- Add file formats with user-supplied importer DLLs/shared libraries; register them through the CLI.
- Open, save, and drag in `.woby` scene files with persisted model paths, scene tree hierarchy, helper visibility, up-axis, render modes, transforms, opacity, color, and vertex-size settings.
- Inspect scenes with mouse and keyboard camera controls for orbit, pan, roll, dolly, local movement, and quick reframe.
- View renderer, FPS, vertex, and triangle counts while working.
- Toggle the origin axes, adaptive ground grid, and Y-up or Z-up scene orientation.
- Show selected mesh dimensions in the scene and inspect their size in Properties. The grid displays its current spacing.
- Control visibility at the scene, folder, file, and mesh-group levels with tri-state master controls.
- Render each group as solid mesh, triangle edges, vertices, or any combination of those modes.
- Adjust global, per-file, and per-group vertex point sizes.
- Arrange files and groups with translation, rotation, scale, opacity, and reset controls.
- Assign and reset per-group display colors.
- Select a folder, file, or part to edit its persistent **Properties** inspector on the right. Transform fields label X/Y/Z and rotation angles; opacity uses percent and scale uses a uniform multiplier. Type a value and press Enter to apply it, or Escape to cancel.
- Ctrl-click multiple objects to edit shared properties. **Mixed** fields have differing local values; editing a field sets only that field on every selected object. Parent transforms compose with part transforms and their opacities multiply. Selecting both a parent and its child edits both. Color and render modes are part properties; selecting a comparison shows its controls in the same Properties pane.
- Reset translation, rotation, scale, all transforms, or appearance independently. Transform resets preserve opacity; appearance resets preserve transforms and unselected child overrides. Geometry shows mesh statistics and local bounds for a single file or part. These edits use the existing `.woby` save/load settings.
- Remove files from a scene without touching the source model files.
- Compare surfaces with a numeric heatmap legend, tolerance, saturation warning, and sampled maximum/mean/P95/area statistics. Distances are approximate and unsigned. Measurements use the mesh coordinates without conversion.
- Load large model batches and scenes in the background with progress, cancellation, and unsaved-change prompts.

## Getting started

Undo and Redo arrow buttons sit beside Settings in the top toolbar.
The file actions are arranged in three rows of two: **New scene** / **Open scene...**,
**Save scene** / **Save scene as...**, and **Add models...** / **Add model folder...**.
New scene starts an untitled scene and asks before discarding unsaved changes.
`Ctrl+O` opens a scene, `Ctrl+S` saves it (asking for a path for an untitled scene),
and `Ctrl+Shift+S` opens Save As. Save As switches to the chosen destination only
after a successful save; canceling keeps the active document unchanged.

Save and Save As also capture the current camera target, orbit, roll, distance,
field of view, and near plane. Opening the file restores that review view.
Camera navigation never marks a scene dirty and is excluded from Undo/Redo;
use Save explicitly to keep a new view, even when no unsaved-change marker appears.
Older scenes without camera data still frame the scene automatically. Resizing
the viewport keeps the logical camera and adapts the field of view to its shorter
axis. Invalid camera numbers are rejected; finite out-of-range values are clamped
when loading.

`Ctrl+Z` undoes a scene edit; `Ctrl+Y` or `Ctrl+Shift+Z` redoes it. This includes
transforms, appearance, comparison membership, and object additions/removals.
A continuous drag is one action. Session history has no action-count limit;
New/Open starts fresh history, while Save keeps history and updates the clean-state
baseline. History stores model paths and settings, not mesh buffers. Restoring a
removed model reloads its source file; changed geometry is accepted when its part
names and order still match. If the source is missing, unreadable, or incompatible,
an error is shown and that Undo/Redo action is skipped, leaving the scene unchanged.
Camera navigation, selection, and app preferences are outside scene history.
Active text fields use their own text undo.
From the CLI, use `woby ctl --instance ID scene undo` or
`woby ctl --instance ID scene redo` to trigger one step in the same history.

Use Add models for OBJ, STL, or installed importer formats, and Open Scene for
saved `.woby` scenes. The empty viewport offers both actions and accepts dropped
model files, folders, and scenes. Add model folder imports models recursively.

The **Objects** tree lists scene content, and **Display** contains global viewing
controls. The Frame icon beside the Y/Z up-axis button fits the scene in view;
the camera icon immediately after it opens PNG export options. Choose resolution,
the scene or visible comparison results, and legend/name/source/direction/threshold
annotations. Comparison exports wait for complete visible results and report an
error if annotations cannot fit; increase the image height or reduce visible results.
Geometry counts and
renderer/FPS details sit at the bottom of the left pane. File actions are disabled
while a dialog or file-processing operation is active; New scene and Screenshot
also wait for outstanding image captures.

New scenes and newly added models start with solid surfaces, with edges, vertices,
grid, and origin helpers off. Existing `.woby` scenes retain their saved appearance,
including the defaults of older scene files. **Display > Inspection presets** offers
**Solid**, **Solid + edges**, and **Solid + edges + vertices** for all current parts;
each hides the helpers while preserving visibility, transforms, colors, and opacity.
The individual display controls remain available. Enabled icon toggles have an
outline, mixed toggles have a minus, and selected objects have an outline, so these states are distinguishable without color.

**Display > Show dimensions** adds labeled dimension lines beside the visible selected
geometry. One visible part is measured along its own directions, including all parent
scaling and rotation; multiple parts are measured together along the scene's fixed
directions. X/Y/Z sizes also appear at the bottom of the viewport, including zero dimensions
and edges that are too small or off-screen to label. **Properties > Geometry** shows
the same sizes, followed by original local bounds for a single file or part.
Enabling the grid displays **Grid spacing** using the same spacing as the drawn lines.
All values are raw coordinates, without unit conversion. Dimension visibility is
saved in `.woby` files and supports Undo/Redo; selection remains transient. Scene
PNG exports include these overlays when enabled (results-only exports omit them).

**Interface > UI scale** adjusts text and controls from 100% to 200%, in addition to
automatic monitor scaling. This preference is saved for the current user and survives
scene changes without dirtying the scene. Panes scroll when needed at larger sizes.
Save/load/export feedback appears in floating toasts at the top of the viewport
and fades away after eight seconds. Contextual tooltips explain
selection, comparison position, input swapping, and units.

Left drag or arrow keys orbit, right/middle drag pans, and the wheel or `+`/`-`
zooms. Alt-left drag rolls. Click a surface, displayed edge, or displayed vertex
to select its part; click a comparison result to select the comparison. Ctrl-click
toggles membership in the selection, and a plain click on empty space clears it.
A small movement threshold separates clicks from drags. Selected geometry has
yellow bounding boxes, including the visible children of selected files/folders.
Edges and vertices have a small click tolerance; x-ray edges remain selectable
through surfaces. Fully transparent and hidden objects are ignored. Partially
transparent surfaces remain selectable, with overlapping results following draw order.
Selection does not change the saved scene or its dirty state.
`R` frames the scene, `Ctrl+B` toggles the left pane, and the upper-right
Properties/pin button toggles the right-hand Properties pane. Selecting an object
opens this pane and shows the properties for that selection. `W`/`A`/`S`/`D` move
the camera, `Q`/`E` move vertically, and Shift speeds movement up. Camera keys are
paused while editing fields, using Ctrl/Alt/Super commands, or displaying dialogs
and popups. Escape dismisses context menus and cancels discard confirmations;
outside editing and popups it clears the selection.

## Command Line

On Windows, double-clicking `woby.exe` opens only the viewer. When launched from
a terminal, Woby attaches to that existing console and preserves redirected
input/output. Startup errors appear in a dialog if no stderr destination exists.
As with other Windows GUI executables, an interactive shell may return its prompt
before Woby exits. In Command Prompt, use `start /wait "" woby.exe ctl instances`
when you need to wait; in PowerShell, capturing or piping output waits for it,
for example `$instances = .\woby.exe ctl instances --json`.

### Comparison objects

Scenes can contain multiple named comparisons alongside the original models.
Select one file, folder, or mesh part, then right-click and choose **Create comparison**
to inspect its surface, triangle edges, boundaries, and non-manifold/winding edges.
Inspection starts with either group populated. Distance measurements and overlay
become available when both groups have inputs. To compare two objects, Ctrl-click
them before creating the comparison: the first supplies **A**, the second **B**.
Clearing one group returns to single-input inspection; adding it back restores
the saved two-input display mode.
Use the scene tree's **Comparison membership** context menu to assign sources
to a named comparison. Group A/B headings show part and triangle counts.
Right-click preserves the existing selection and includes the clicked object;
context-menu membership actions apply to all selected sources.

Each comparison has independent visibility, tolerance, color range, A/B display
mode, edge diagnostics, and **Result position**. Select its tree row to edit its
properties. The **x** at the end of its row deletes the comparison without removing
its source objects. Right-click the row to duplicate, frame, or delete it. New results
are placed beside existing results. Result position is a display offset: moving
it never changes measured distances or source transforms. Normal scene meshes
continue to render with their own settings.

Scenes without a saved comparison position, including older `.woby` files, receive
automatic spacing when opened. Saved positions, including an intentional zero
offset, are preserved when reopening.

Files and folders contribute their current triangular parts, deduplicated per
side. Parts can belong to both sides and to multiple comparisons. Future children
are not automatically included. Source visibility and appearance do not affect
measurements. Deleting a source leaves a named missing reference; repair the
inputs or explicitly remove missing references before computing again. Deleting
a comparison never deletes its sources.

The upper-right Properties button toggles the inspector. Its A/B trees show source
names and part counts and retain the source hierarchy. Collapse or expand a group,
right-click a branch or part to remove it, or right-click a group heading and choose
**Clear group A** / **Clear group B**. Missing-input
messages identify the side and any unavailable references. **Swap inputs A / B**
exchanges assignments and keeps the chosen measurement direction. **A -> B** colors
A by its nearest distance to B; **B -> A** colors B by its nearest distance to A.
The direction controls name the measured (heatmap) and reference surfaces. These
controls affect only the inspected comparison. Part badges in the scene tree
show membership for the last active comparison.

Comparison uses combined surfaces at their source scene positions, with
bidirectional unsigned distance heatmaps, A/B views, a wireframe overlay, and edge
diagnostics. It does not perform a Boolean union. Up to two comparisons compute
concurrently; screenshots wait for all visible results and report incomplete
inputs or computation errors instead of exporting a partial comparison scene.

Version 5 `.woby` scenes save comparison objects and source references. Version
2Ã¢â‚¬â€œ4 scenes remain readable; existing A/B memberships migrate into one comparison
at the original source positions. Version 6 adds the optional camera record and
still reads versions 2–5. Older woby builds cannot read version 6 scenes.
Select **Surface mesh quality** in a comparison's display selector to inspect either
A or B, including comparisons with only one input. Choose **Longest edge**,
**Equivalent size** (edge length of an equilateral triangle with the same area),
**Shape quality** (1 = equilateral), or **Local size jump** (largest equivalent-size
ratio across neighboring triangles). A/B share color ranges and histogram bins;
the histogram shows percentages with a shared vertical scale. Tables report
minimum/P5/median/P95/maximum, worst shape, maximum size jump, and face counts.
Optional inclusive minimum/maximum limits apply to the longest edge and report
both the percentage of valid triangles and the percentage of their area outside
the limits. The size-limit controls and their statistics are grouped together. Settings persist in `.woby`
and support undo/redo; screenshot annotations include quality measurements.

Metrics use source triangles at their scene transforms. Degenerate triangles
are reported separately and excluded from statistics. Local size jumps use exact
position matching across edges with two valid incident faces; boundary and
non-manifold edges are excluded. Faces without a valid neighbor are unavailable
(gray); degenerate faces use magenta where drawable. Percentiles give each valid
face equal weight and interpolate between sorted values. These are geometric
surface measurements; they do not certify FEM accuracy or volume mesh quality.

Five ready-to-open [surface mesh quality sample projects](assets/samples/surface-mesh-quality/README.md)
cover coarse/fine density, equal-area shape differences, gradual/abrupt grading,
size-limit percentages, and topology edge cases. Each includes expected values
and starts with the relevant heatmap selected.

Open `assets/samples/mesh-comparison/compare.woby` for a before/after repair example.

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene .\assets\samples\mesh-comparison\compare.woby
```

See the [sample walkthrough and prototype limits](assets/samples/mesh-comparison/README.md).

Comparisons can also be created and edited through the running viewer's local server
with `woby.exe ctl --instance ID comparison create|set|add|remove|clear|swap|delete`.
`comparison results COMPARISON_ID --json` waits for mesh diagnostics and, when both
inputs are populated, measurements in both directions including maximum/mean/P95
distance and area above tolerance. With one input, unavailable distance metrics
and the absent side are `null`.
See the [comparison CLI reference](doc/ctl-commands.md#comparisons) for a complete example.

Run the app:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe
```

Load OBJ files:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --file C:\path\to\model.obj
.\build\vs2026-vcpkg\bin\Debug\woby.exe --file C:\path\to\a.obj --file C:\path\to\b.obj
```

Load all OBJ files in a folder recursively:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --folder C:\path\to\models
```

Load a folder recursively and mirror nested directories in the scene tree:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --folder-tree C:\path\to\models
```

Open a saved scene:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene C:\path\to\scene.woby
.\build\vs2026-vcpkg\bin\Debug\woby.exe --woby C:\path\to\scene.woby
```

Open a scene and add OBJ files to it:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene C:\path\to\scene.woby --file C:\path\to\model.obj
```

Load importer plugins and a model in an additional format:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --plugin C:\plugins\off.dll --plugin D:\tools\other.dll --file C:\models\part.off
.\build\vs2026-vcpkg\bin\Debug\woby.exe --plugin-folder C:\plugins --plugin-folder D:\company-plugins
```

Plugin folders are scanned non-recursively. CLI registrations apply to the current launch;
registrations added through `wobyctl importers add PATH --remember` are remembered. See [the importer guide](doc/importers.md)
for the C API, scene persistence rules, and a buildable OFF importer example.

Print the application version:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --version
```

Update a portable release installation from GitHub:

```powershell
woby update --check
woby update
woby update --status
```

Close viewers using that installation before updating. The updater downloads the
latest stable package for the executable's platform, verifies SHA-256 and the
package manifest, and replaces package-owned files while preserving unrelated
scenes and plugins. An external helper finishes installation after the CLI exits;
exit code `2` means pending, so use `--status` to confirm completion. All three
commands accept `--json`. See [updates and recovery](doc/updates.md).

The first release containing the updater and `woby-manifest.json` must be installed
manually. Older releases and development build folders cannot update themselves;
`--check` still works from a build. Self-update supports writable, per-user portable
installations; it does not elevate privileges or update package-manager installs.

Enable file logging:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --log-level info --log-file C:\path\to\woby.log
```

Logging is off by default. Supported log levels are `off`, `trace`, `debug`, `info`, `warn`, `error`, and `critical`.
At `info`, `debug`, or `trace`, the log includes startup, scene, folder scan, and OBJ load performance entries.

Enable frame performance logging:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --log-level info --log-file C:\path\to\woby.log --log-performance
.\build\vs2026-vcpkg\bin\Debug\woby.exe --log-level info --log-file C:\path\to\woby.log --log-performance --log-frame-interval 60 --log-slow-frame-ms 20
```

Frame performance logging is opt-in. `--log-frame-interval` controls how many frames are summarized per log entry, and defaults to `120`. `--log-slow-frame-ms` logs individual frames whose total duration exceeds the provided threshold.

## Control a running instance

Every Woby viewer automatically starts a local HTTP API. Its generated instance ID appears
in the window title, for example `woby [woby-3c981bd42b76e80f] - untitled`.
You can choose a custom ID when starting the viewer:

```powershell
woby.exe --instance review --file C:\models\part.obj
```

IDs are unique among running instances for the current user. They contain 1-64 lowercase
letters, digits, hyphens or underscores, starting with a letter or digit. A duplicate ID
fails startup instead of connecting to or replacing the existing viewer. Custom IDs can
be reused after an instance exits. IDs belong to the running process and are not saved in `.woby` files.

From another terminal, discover instances and save the current scene as a PNG:

```powershell
woby.exe ctl instances
woby.exe ctl instances --json
woby.exe ctl --instance review screenshot C:\output\view.png
woby.exe ctl --instance review screenshot .\view.png --timeout 120 --json
```

`ctl` connects to an existing instance and never creates a viewer. Screenshot commands
always wait until the PNG is written; `--wait` is accepted but optional. The default
timeout is 60 seconds (`--timeout` accepts 1-3600). Relative output paths are resolved
against the CLI's working directory. Parent directories are created and the extension
is normalized to `.png`. Existing files are overwritten, matching the screenshot UI.
The capture uses the existing 1920 Ãƒâ€” 1800 scene-only renderer, including scene helpers
but excluding the application controls. It uses the current camera.

Exit code `0` means success; `1` means failure. `--json` emits one JSON value on stdout,
including `{"error":"..."}` for command failures. Instance listing returns an array;
capture returns `{"instance":"review","path":"C:\\output\\view.png"}` only after saving.
Commands queue in admission order, with at most eight active/queued commands total.
Captures fail explicitly if the instance is starting, processing model files, or the
UI screenshot pipeline is busy. After a timeout, queued work is canceled; a capture already submitted to the
GPU may still finish and save. Closing the viewer releases waiting clients.

See [the local HTTP API](doc/automation.md) for direct scripting and discovery details.

CTL also supports scene inspection, visibility/render modes, transforms, appearance,
camera navigation, model and importer management, diagnostics, and pane controls.
See the [complete command reference](doc/ctl-commands.md), or run `woby ctl --help`.
Run `woby.exe --help` for command syntax.

Discover loaded folders, files, and mesh groups, then resolve an object by its ID:

```powershell
woby.exe ctl --instance review objects --json
woby.exe ctl --instance review object OBJECT_ID --json
```

Use an ID returned by `objects`; names and paths are not identifiers. IDs remain stable
while objects stay loaded, even when another file is removed and indices shift. Removed
objects, reopened scenes, and restarted viewers invalidate old IDs. IDs are not saved
in `.woby` files. See [object discovery and lifetime](doc/automation.md#discover-and-resolve-scene-objects)
for response fields and stale-ID errors.

Scene commands return an admission `sequence` as a decimal string. They execute on
the main thread in FIFO order through a bounded queue; a screenshot finishes writing
before the next command executes. Automation assumes one coordinating client and no
manual scene edits during a workflow. Concurrent clients and UI input are serialized
safely, but their effects may interleave. See [command ordering](doc/automation.md#command-ordering-and-concurrency)
for queue, timeout, and concurrency semantics.

## Build

### macOS application bundle

macOS builds produce `woby.app`. Double-click the app in Finder to launch the
viewer without Terminal. Assets, including fonts and compiled shaders, live
inside the bundle. The macOS release archive contains this app; it can be moved
to `/Applications` or another folder.

The command-line interface is also available through the bundled executable:

```sh
/Applications/woby.app/Contents/MacOS/woby --version
/Applications/woby.app/Contents/MacOS/woby ctl instances
```

To prepare a distributable bundle from a local macOS build, run
`cmake --install <build-directory> --config Debug --prefix <destination> --component Runtime`
after building. Use `--config Release` for a Release build. Installation embeds
required non-system libraries, repairs their paths, and applies an ad-hoc
signature. This does not provide Developer ID signing or Apple notarization.

### Python tooling

Tests and release packaging require [uv](https://docs.astral.sh/uv/getting-started/installation/)
on Windows, Linux, and macOS. The supported Python interpreter is pinned exactly
in `.python-version` (currently 3.13.14); `uv.toml` selects uv-managed Python.
All Python scripts use the standard library, so no pip packages or virtual
environment activation are needed.

Run `uv python install` from the repository root before configuring. CMake uses
uv to locate the pinned interpreter, replaces stale Python selections in existing
build caches, and reports an error if that version is missing. Configuration
does not download Python. With `BUILD_TESTING=OFF`, Woby's CMake configuration
does not require Python or uv; release packaging still does.

### Windows development

Set `VCPKG_ROOT` to your vcpkg checkout, then configure and build the Debug preset:

```powershell
$env:VCPKG_ROOT="C:\path\to\vcpkg"
uv python install
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
```

Run tests:

```powershell
ctest --preset vs2026-vcpkg
```

To check annotated screenshots immediately after startup, run this regression on
a machine with a desktop. It launches four fresh viewers and saves their PNGs and
logs in the output directory:

```powershell
uv run tests/ctl_startup_screenshot_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe build/startup-capture-qa
```

Use `uv run path/to/script.py ...` for all standalone Python scripts and
`uv run python --version` to check the selected interpreter. CI installs uv and
runs `uv python install` in each build or release job that needs Python, then
uses the same pin for tests and packaging. Update `.python-version`, rerun
`uv python install`, and reconfigure CMake when upgrading Python.

The Visual Studio presets build two projects concurrently and use MSBuild's
shared compiler worker pool across them. To limit compiler workers on a smaller
machine, append `-- /p:MultiProcMaxCount=8` to the build command. CI continues to
use Ninja's own parallel scheduling.

Stable headers are precompiled separately for the app, automation library, and
tests. To check compilation without precompiled headers, configure with
`cmake --preset vs2026-vcpkg -DWOBY_USE_PCH=OFF`; set it back to `ON` to restore
the default.

Builds stage changed assets and shaders and repair missing runtime files.
Removed assets are removed from the staged directory; unrelated files are
preserved. Windows DLLs are copied from CMake's transitive runtime dependency
list, without repeated PowerShell dependency scans. Test discovery is cached
per configuration and refreshed by CTest when the executable or discovery
configuration changes, including after building just `woby_tests`.

See [CI builds and releases](doc/ci.md) for dependency caching, the pinned CI
toolchain, and reuse of main-build packages when publishing a version tag.
