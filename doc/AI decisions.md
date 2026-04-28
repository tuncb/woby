# AI decisions

## 2026-04-28: macOS arm64 GitHub Actions pipeline

- Added `woby-macos-arm64` to the existing build/release matrix instead of creating a separate workflow, so macOS follows the same checkout, vcpkg, CMake, build, package, artifact, and tagged-release path as Windows and Linux.
- Used GitHub's `macos-latest` runner label because the current GitHub-hosted runner documentation maps that label to macOS arm64 Apple Silicon runners. The package name still spells out `arm64` so release assets remain explicit if GitHub changes alias behavior later.
- Used the built-in vcpkg `arm64-osx` triplet. The project only needs a custom overlay triplet for Linux dynamic linkage and SDL options today, and adding an overlay for macOS would duplicate vcpkg defaults without a project-specific setting.
- Kept CI on Ninja plus vcpkg. The local Visual Studio preset rule remains unchanged for developer builds; the workflow continues using explicit CMake configure arguments because that is the existing CI pattern.
- Added a macOS staging path that uses `otool` and `install_name_tool` for workspace-built dylib dependencies instead of Linux `ldd`, so the packaged executable can find copied vcpkg dylibs through local `@executable_path` and `@loader_path` run paths.

## 2026-04-28: Unit-testable UI operations

- Created a canonical logical UI state in `src/ui_state.*`. GPU handles, SDL dialogs, toast timing, and other OS/runtime resources remain outside `woby::UiState` because they are not deterministic unit-test state.
- Split file data from GPU resources. `woby::UiState::files` owns paths, meshes, visibility, transforms, colors, and vertex-size settings; `LoadedObjRuntime` in `main.cpp` owns the matching bgfx resources.
- Put state-changing UI behavior in `src/ui_operations.*` as free functions over structs. The ImGui layer now edits local values and calls operations for visibility, render modes, transforms, colors, vertex sizes, pane width, bounds recalculation, and file removal.
- Centralized clamping in the operation layer. Non-finite numeric input falls back to a stable default so malformed scene data or direct text entry cannot leave NaN/Inf in saved UI state.
- Kept `SceneDocument` as the persisted file format and moved UI-state mapping into `ui_state.*`, so tests can verify that user-editable fields round-trip into the `.woby` document model.
- Split SDL/ImGui keyboard polling into `src/camera_sdl.cpp`; `src/camera.cpp` now contains camera math only, which keeps camera-dependent state operations testable without pulling in SDL event state.
- Scene submission now happens after the ImGui controls update `woby::UiState`, so state changes made by UI operations are used by the scene draw in the same frame.
