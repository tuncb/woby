# Task completion rules

## Completeness rules

- Add unit tests for new features and behavioral changes unless you need to change implementation structure to make it testable, do not add tests in this case but report it to the user.

- Run unit tests and make sure they compile without warnings and all tests pass.

## Filesystem test rules

- Assume the repository/build directory and the system temporary directory are on different Windows drives. Never rely on them sharing a drive or on the test's current working directory.
- For save/load and relative-path tests, derive all related paths (scene files, model files, and folder roots, including synthetic fixture paths) from one unique temporary directory per test. Pass absolute paths unless relative input is specifically under test; in that case, establish an explicit base directory on the same drive.
- Clean up temporary fixtures after each test, including on failure. Test cross-drive rejection separately when intentional; do not weaken production path validation to accommodate a fixture.

## Version update workflow

- When the user asks for a version update, update the canonical application version in both `CMakeLists.txt` (`project(woby_obj_viewer VERSION ...)`) and `vcpkg.json` (`version-string`); keep the values identical.
- Validate the release version and tag with `uv run .github/scripts/package_manifest.py validate-tag vX.Y.Z`.
- If there are no unrelated changes in the repository, commit the version update and wait for the commit to finish. Only after the commit succeeds, create the matching tag with `nu c:\tools\gittag.nu vX.Y.Z`.

## Code changes
- Build woby project in debug mode, the build should succeed without any warnings.
- If there is already a woby instance open, terminate the process and re-try.

# Build rules

- For local development, use vs2026-vcpkg preset instead of ninja + vcpkg.
- CI flow should depend on ninja + vcpkg.

# Architectural rules

woby::UiState is the canonical logical UI state. User-visible scene controls should live there or in structs owned by it.
bgfx, SDL, ImGui, dialogs, and timing state must stay outside UiState; keep them in runtime/adaptor code.
UI code should read state, edit local values, then call ui_operations functions. It should not directly mutate scene-visible state.
Any new user-editable scene property must be handled in three places: UiState, ui_operations, and .woby save/load mapping.
Transformation functions should be deterministic, free functions over structs, with no ImGui/SDL/bgfx dependencies.
Clamp and validate state at operation/load boundaries, not in rendering code.
Render code should only draw from the already-updated state; it should not change logical state.

# Code style rules

- Use procedural programming style. Structs + free functions.
- No inheritance, no private or protected members.

# UI rules

- Use eye icon for showing visibility state.
- Use settings icon for showing settings for detectors
- If a button opens a dialog box the dialog box should be opened near the button.
