# woby OBJ Viewer

CMake-based C++ starter project for rendering Wavefront OBJ meshes with SDL3,
bgfx, tinyobjloader, meshoptimizer, and Dear ImGui.

## Dependencies

The intended setup is vcpkg manifest mode. Install vcpkg, then set `VCPKG_ROOT`
to the vcpkg checkout directory.

```powershell
$env:VCPKG_ROOT="C:\path\to\vcpkg"
cmake --preset vs2026-vcpkg
cmake --build --preset vs2026-vcpkg
.\build\vs2026-vcpkg\bin\Debug\woby.exe
```

Pass OBJ paths to load one or more models:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --file C:\path\to\model.obj
.\build\vs2026-vcpkg\bin\Debug\woby.exe --file C:\path\to\a.obj --file C:\path\to\b.obj
```

Load every OBJ file recursively inside a folder:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --folder C:\path\to\models
```

Open a saved woby scene:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --woby C:\path\to\scene.woby
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene C:\path\to\scene.woby
```

You can combine a scene with OBJ inputs. The scene opens first, then the OBJ
files are added to it, leaving the scene dirty:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --scene C:\path\to\scene.woby --file C:\path\to\model.obj
```

You can also drag `.obj` files or folders onto the window to add them to the
current scene. Dragged folders are scanned recursively. Drag a single `.woby`
scene file by itself to open it with the same unsaved-change prompt as the UI
open command.

Print the application version:

```powershell
.\build\vs2026-vcpkg\bin\Debug\woby.exe --version
```

The Ninja presets are also available, but they must be run from a shell where
`cl.exe` is already on `PATH`, such as a Visual Studio Developer PowerShell:

```powershell
cmake --preset ninja-vcpkg
cmake --build --preset ninja-vcpkg
```

If dependencies are installed some other way, use the plain Ninja preset and
make sure CMake can find each package:

```powershell
cmake --preset ninja
cmake --build --preset ninja
```

## Project Layout

- `src/main.cpp` creates the SDL3 window, initializes bgfx, and runs the frame loop.
- `src/obj_mesh.*` loads OBJ files with tinyobjloader and optimizes buffers with meshoptimizer.
- `src/imgui_bgfx.*` renders Dear ImGui draw data through bgfx.
- `cmake/BgfxShaders.cmake` compiles bgfx shaders into runtime assets.
- `assets/models/cube.obj` is a small sample model.

## Notes

bgfx shaders are compiled at build time with `shaderc`, provided by the `bgfx`
vcpkg `tools` feature. The app loads shaders from `assets/shaders/<renderer>/`
next to the executable.
