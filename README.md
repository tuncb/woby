# woby

woby is a desktop OBJ scene viewer for loading, inspecting, arranging, and saving Wavefront OBJ models.

## Features

- Load one or more `.obj` model files.
- Load every `.obj` file in a folder recursively.
- Drag `.obj` files, `.woby` scenes, or folders into the window.
- Inspect scenes with orbit, pan, and zoom camera controls.
- Toggle model, group, grid, and origin visibility.
- Show solid mesh, triangle edges, and vertices.
- Edit file and group transforms, opacity, color, and vertex point size.
- Save and reopen `.woby` scene files.

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
