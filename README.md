# woby

woby is a desktop OBJ scene viewer for loading, inspecting, arranging, and saving Wavefront OBJ models.

## Features

- Load Wavefront OBJ and STL model files from the UI, command line, drag and drop, or recursive folder import.
- Open, save, and drag in `.woby` scene files with persisted model paths, helper visibility, up-axis, render modes, transforms, opacity, color, and vertex-size settings.
- Inspect scenes with mouse and keyboard camera controls for orbit, pan, roll, dolly, local movement, and quick reframe.
- View renderer, FPS, vertex, and triangle counts while working.
- Toggle the origin axes, adaptive ground grid, and Y-up or Z-up scene orientation.
- Control visibility at the scene, file, and mesh-group levels with tri-state master controls.
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

Open a saved scene:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene C:\path\to\scene.woby
.\build\vs2026-vcpkg\bin\Debug\woby.exe --woby C:\path\to\scene.woby
```

Open a scene and add OBJ files to it:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene C:\path\to\scene.woby --file C:\path\to\model.obj
```

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
