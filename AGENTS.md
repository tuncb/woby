# Task completion rules

## Completeness rules

- Add unit tests for new features and behavioral changes unless you need to change implementation structure to make it testable, do not add tests in this case but report it to the user.

- Run unit tests and make sure they compile without warnings and all tests pass.

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
