# woby OBJ Viewer

CMake-based C++ starter project for rendering Wavefront OBJ meshes with SDL3,
bgfx, tinyobjloader, meshoptimizer, and Dear ImGui.

## Dependencies

The intended setup is vcpkg manifest mode. Install vcpkg, then set `VCPKG_ROOT`
to the vcpkg checkout directory.

```powershell
cmake --preset ninja-vcpkg
cmake --build --preset ninja-vcpkg
.\build\ninja-vcpkg\bin\woby_viewer.exe
```

Pass an OBJ path to load another model:

```powershell
.\build\ninja-vcpkg\bin\woby_viewer.exe C:\path\to\model.obj
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
- `assets/models/cube.obj` is the default model.

## Notes

bgfx shaders are compiled at build time with `shaderc`, provided by the `bgfx`
vcpkg `tools` feature. The app loads shaders from `assets/shaders/<renderer>/`
next to the executable.
