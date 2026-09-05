# woby

woby is a desktop OBJ scene viewer for loading, inspecting, arranging, and saving Wavefront OBJ models.

## Features

- Load Wavefront OBJ and STL model files from the UI, command line, drag and drop, or recursive folder import.
- Add file formats with user-supplied importer DLLs/shared libraries; register them in the Importers panel or through repeatable `--plugin` and `--plugin-folder` options.
- Open, save, and drag in `.woby` scene files with persisted model paths, scene tree hierarchy, helper visibility, up-axis, render modes, transforms, opacity, color, and vertex-size settings.
- Inspect scenes with mouse and keyboard camera controls for orbit, pan, roll, dolly, local movement, and quick reframe.
- View renderer, FPS, vertex, and triangle counts while working.
- Toggle the origin axes, adaptive ground grid, and Y-up or Z-up scene orientation.
- Control visibility at the scene, folder, file, and mesh-group levels with tri-state master controls.
- Render each group as solid mesh, triangle edges, vertices, or any combination of those modes.
- Adjust global, per-file, and per-group vertex point sizes.
- Arrange files and groups with translation, rotation, scale, opacity, and reset controls.
- Assign and reset per-group display colors.
- Remove files from a scene without touching the source model files.
- Load large model batches and scenes in the background with progress, cancellation, and unsaved-change prompts.

## Command Line

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
registrations added through the Importers panel are remembered. See [the importer guide](doc/importers.md)
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
The capture uses the existing 1920 × 1800 scene-only renderer, including scene helpers
but excluding the application controls. It uses the current camera.

Exit code `0` means success; `1` means failure. `--json` emits one JSON value on stdout,
including `{"error":"..."}` for command failures. Instance listing returns an array;
capture returns `{"instance":"review","path":"C:\\output\\view.png"}` only after saving.
Commands queue in admission order, with at most eight active/queued commands total.
Captures fail explicitly if the instance is starting, processing model files, or the
UI screenshot pipeline is busy. After a timeout, queued work is canceled; a capture already submitted to the
GPU may still finish and save. Closing the viewer releases waiting clients.

See [the local HTTP API](doc/automation.md) for direct scripting and discovery details.
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
