# woby

woby is a desktop OBJ scene viewer for loading, inspecting, arranging, and saving Wavefront OBJ models.

## Features

- Load Wavefront OBJ and STL model files from the UI, command line, drag and drop, or recursive folder import.
- Add file formats with user-supplied importer DLLs/shared libraries; register them through the CLI.
- Open, save, and drag in `.woby` scene files with persisted model paths, scene tree hierarchy, helper visibility, up-axis, render modes, transforms, opacity, color, and vertex-size settings.
- Inspect scenes with mouse and keyboard camera controls for orbit, pan, roll, dolly, local movement, and quick reframe.
- View renderer, FPS, vertex, and triangle counts while working.
- Toggle the origin axes, adaptive ground grid, and Y-up or Z-up scene orientation.
- Control visibility at the scene, folder, file, and mesh-group levels with tri-state master controls.
- Render each group as solid mesh, triangle edges, vertices, or any combination of those modes.
- Adjust global, per-file, and per-group vertex point sizes.
- Arrange files and groups with translation, rotation, scale, opacity, and reset controls.
- Assign and reset per-group display colors.
- Select a folder, file, or part to edit its persistent **Properties** inspector on the right. Transform fields label X/Y/Z in model units and degrees; opacity uses percent and scale uses a uniform multiplier. Type a value and press Enter to apply it, or Escape to cancel.
- Ctrl-click multiple objects to edit shared properties. **Mixed** fields have differing local values; editing a field sets only that field on every selected object. Parent transforms compose with part transforms and their opacities multiply. Selecting both a parent and its child edits both. Color and render modes are part properties; selecting a comparison shows its controls in the same Properties pane.
- Reset translation, rotation, scale, all transforms, or appearance independently. Transform resets preserve opacity; appearance resets preserve transforms and unselected child overrides. Geometry shows mesh statistics and local bounds for a single file or part. These edits use the existing `.woby` save/load settings.
- Remove files from a scene without touching the source model files.
- Load large model batches and scenes in the background with progress, cancellation, and unsaved-change prompts.

## Getting started

The top actions are arranged in three rows of two: **New scene** / **Open Scene...**,
**Save Scene** / **Save Scene as...**, and **Add models...** / **Add model folder...**.
New scene starts an untitled scene and asks before discarding unsaved changes.
Save Scene updates the current file (or asks for a path for an untitled scene);
Save Scene as always asks for a destination.

Use Add models for OBJ, STL, or installed importer formats, and Open Scene for
saved `.woby` scenes. The empty viewport offers both actions and accepts dropped
model files, folders, and scenes. Add model folder imports models recursively.

The **Objects** tree lists scene content, and **Display** contains global viewing
controls. The Frame icon beside the Y/Z up-axis button fits the scene in view;
the camera icon immediately after it saves a screenshot. Geometry counts and
renderer/FPS details sit at the bottom of the left pane. File actions are disabled
while a dialog or file-processing operation is active; New scene and Screenshot
also wait for outstanding image captures.

Left drag or arrow keys orbit, right/middle drag pans, and the wheel or `+`/`-`
zooms. `R` frames the scene, `Ctrl+B` toggles the left pane, and the upper-right
Properties/pin button toggles the right-hand Properties pane. Selecting an object
opens this pane and shows the properties for that selection.

## Command Line

### Comparison objects

Scenes can contain multiple named comparisons alongside the original models.
Ctrl-click two files, folders, or mesh parts, then right-click and choose
**Create comparison**. The first selected object supplies **A**, the second **B**.
Alternatively, use **New comparison** in the scene tree. Drag sources onto the
**Group A** / **Group B** headings in Properties, or use the scene tree's
**Comparison membership** context menu to assign sources to a named comparison.
Right-click preserves the existing selection and includes the clicked object;
context-menu membership actions apply to all selected sources.

Each comparison has independent visibility, tolerance, color range, A/B display
mode, edge diagnostics, and **Result position**. Select its tree row to edit its
properties. Right-click the row to duplicate, frame, or delete it. New results
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
at the original source positions. Older woby builds cannot read version 5 scenes.
Open `assets/samples/mesh-comparison/compare.woby` for a before/after repair example.

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene .\assets\samples\mesh-comparison\compare.woby
```

See the [sample walkthrough and prototype limits](assets/samples/mesh-comparison/README.md).

Comparisons can also be created and edited through the running viewer's local server
with `woby.exe ctl --instance ID comparison create|set|add|remove|clear|swap|delete`.
`comparison results COMPARISON_ID --json` waits for measurements in both directions,
including maximum/mean/P95 distance, area above tolerance, and mesh diagnostics.
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

Set `VCPKG_ROOT` to your vcpkg checkout, then configure and build the Debug preset:

```powershell
$env:VCPKG_ROOT="C:\path\to\vcpkg"
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
```

Run tests:

```powershell
ctest --preset vs2026-vcpkg
```

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
