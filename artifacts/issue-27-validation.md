# Issue 27 validation

- Debug build succeeded without warnings: `cmake --build --preset vs2026-vcpkg` (see `issue-27-build.log`).
- All 220 tests passed (107.64 seconds): `ctest --preset vs2026-vcpkg` (see `issue-27-tests.log`).
- Windows desktop check on the mesh-comparison sample: UI scales 100%, 150%, and 200%; controls and comparison fields remain readable, with scrolling on shorter panes.
- Automated layout coverage combines 100%, 125%, 150%, 175%, and 200% monitor scales with every supported UI scale. It also checks high pixel density coordinates and a one-pixel viewport. Windows system display settings were not changed during this check.
- Existing sample appearance survived opening. Selecting Solid removed source edges/vertices; the comparison kept its own settings. The scene became dirty, then Ctrl+S saved a disposable review copy and showed feedback below both inspectors (see `issue-27-feedback-150.png`).
- The control API confirmed new scenes are clean with grid/origin off, and newly added models have solid on, edges/vertices off.
- Scale preference is stored separately from scene settings; restored to the original 100% after inspection.
- The review process was stopped before the final build. The temporary review scene and runtime ImGui layout changes were removed.

## Selection row follow-up

- Removed Selection help text, bounded selected object outlines before the delete column, and gave comparison name buttons the same height as eye/delete buttons.
- Debug build succeeded without warnings (`issue-27-rows-build.log`).
- Ran all 223 tests: 222 passed initially; the new outline test needed a 0.001-pixel floating-point tolerance at the antialiasing boundary. After rebuilding, that test passed on rerun (`issue-27-rows-tests.log`, `issue-27-rows-retest.log`).
- Regression coverage checks actual ImGui row geometry at 75%, 100%, 150%, and 200%, and outline vertices against the file-name clip boundary.
- Windows desktop inspection of the sample scene confirmed the complete right edge on selected files, matching comparison/eye/delete heights, and removal of Selection help. The isolated review instance was stopped without saving the scene.

## Toast follow-up

- Restored floating notifications at the top of the viewport, with eight-second duration and fade-out. Removed the permanent feedback strip and Ready text; viewport and inspectors use the full window height again. This supersedes the earlier feedback-strip checks above.
- Updated viewport regression coverage to verify the bottom pixel remains usable at every supported monitor and UI scale.
- Debug build succeeded without warnings (`issue-27-toast-build.log`). Toast display and expiry are checked in the desktop app because isolating the runtime rendering for a unit test would require restructuring it.
- All 223 tests passed in 107.18 seconds (`issue-27-toast-tests.log`). Desktop verification confirmed the Created new scene toast appears and expires without leaving any status bar; the review instance was closed.

## Empty comparison hint

- Added muted, wrapping guidance under Comparisons: Right-click an object and choose Create comparison to add a comparison.
- Desktop verification covered the empty list, creating a comparison (hint hidden), and deleting the last comparison (hint restored).
- Debug build succeeded without warnings (`issue-27-empty-hint-build.log`). No new unit test was added: isolating this ImGui rendering would require restructuring the comparison view; its behavior was checked in the app instead.
- All 223 tests passed in 105.43 seconds (`issue-27-empty-hint-tests.log`).
