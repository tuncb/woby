**Woby UI review — 6 September 2026**

Woby's core interactions work, but important functionality takes too much discovery. The largest improvements would come from protecting the usable viewport, making primary actions readable, and giving selection and comparison setup clearer interfaces.

I used the Windows Debug application with Computer Use, starting from an empty scene. I imported `original.obj` and `repaired.obj` from the repository samples, selected and expanded tree objects, edited and reset a translation, toggled visibility/triangle edges/grid, orbited and reframed the camera, created a comparison, assigned both inputs, switched measurement direction and overlay mode, focused a diagnostic edge, saved/reopened a scene, and exported a PNG. I also checked Ctrl+Z, Escape in the transform popup, and Ctrl+B.

This is a hands-on review of a small sample scene, supplemented by source inspection at commit `3b3e615`. It is not a user study, a large-model performance assessment, or a completed accessibility/DPI audit. Attempts to resize through window automation did not produce a verified smaller window, so no finding depends on a compact-window test.

**Recommended order**

| Priority | Improvement | Why it matters |
| --- | --- | --- |
| High | Reserve a real viewport between the panels | Geometry currently disappears underneath controls. |
| High | Make primary actions and text easier to read | The opening screen relies heavily on small, unfamiliar icons. |
| High | Add a persistent selection inspector | Appearance and transforms are buried in tree rows and popups. |
| High | Add undo/redo and standard document commands | Small exploratory edits should be easy to recover from. |
| High | Put A/B input assignment directly in the comparison inspector | A core workflow currently requires nested context menus. |
| High for shared results | Add a numeric heatmap legend and export context | A comparison PNG currently cannot explain its own colors. |
| Medium | Establish a calmer visual hierarchy | Controls, diagnostics, and model overlays compete for attention. |
| Medium | Improve diagnostic navigation and saved views | Inspection should be easy to continue and reproduce. |

**1. Reserve a real viewport between the panels**

Observed: during orbiting, parts of the model extended under the left sidebar. After completing comparison input B, the newly offset result appeared partly behind the right inspector. The translucent panels expose faint geometry underneath their text. “Frame result” helps recover the comparison, and Ctrl+B successfully hides the left pane, but both add work.

Recommended: use opaque panels and calculate the camera projection, framing, picking, and navigation from the remaining viewport rectangle. Preserve a useful view as panels open or close. After a comparison becomes ready, make “Show result” obvious and offer a result-only view that preserves the source visibility settings.

Acceptance check: with either or both panels open, Frame scene/Frame selection/Frame result place the complete target within the usable canvas. Picking and orbiting agree with that canvas.

Evidence: [result partly behind inspector](C:/work/woby/artifacts/ui-review-2026-09-06/05-result-obscured.png). Source: [full-window rendering rectangle and projection](C:/work/woby/src/main.cpp:3610).

![Comparison result initially partly hidden behind the inspector](C:/work/woby/artifacts/ui-review-2026-09-06/05-result-obscured.png)

**2. Make primary actions and text easier to read**

Observed: the empty view contains a grid and a small icon toolbar, without a prominent import action or a useful explanation of what to do next. “Scene” appears both above global controls and above the object tree. Renderer/FPS counts are more prominent than navigation guidance. Tooltips do explain the icons, but require deliberate discovery.

Recommended: add labeled **Add models**, **Open scene**, **Save**, and **Export image** actions. Use an empty-state message with import/open buttons and a short supported-format/drop hint. Rename the tree section **Objects**. Put performance information in a quiet status bar or optional diagnostics panel. Provide a visible Frame button and a Help control for orbit, pan, zoom, and keyboard shortcuts.

For visual styling, use a readable proportional UI font and reserve monospace for numbers. Add a UI-scale preference and validate effective text/control sizes at common Windows scaling settings. A practical starting point is 15–16 logical-pixel body text with 30–36-pixel primary controls, then verify on real displays. These are proposed design values, not measured accessibility compliance.

Evidence: [empty scene](C:/work/woby/artifacts/ui-review-2026-09-06/01-empty-scene.png). Source: [font/style constants](C:/work/woby/src/main.cpp:65), [primary toolbar](C:/work/woby/src/main.cpp:3175).

**3. Give selection a persistent properties inspector**

Observed: clicking the file row highlights it. Expanding it reveals another row of icons, and its child surface has visibility, render modes, size, color, and transform controls packed into one line. Transform editing opens a small popup over the viewport. Its three translation/rotation fields have no visible X/Y/Z labels. Dragging translation worked, and Reset returned it to zero.

Recommended: keep the tree focused on hierarchy, name, visibility, and a small amount of state. Selecting an object should show its name/type and **Transform**, **Appearance**, and **Geometry** sections in a consistent inspector. Label X/Y/Z explicitly, show model units/degrees, make direct numeric entry discoverable, and provide reset per property group. Clearly indicate whether the target is a file, part, folder, or multiple objects.

Acceptance check: a user can select a part, identify its parent, enter a precise translation, change opacity, and reset only the intended property without opening an unlabeled icon popup.

Evidence: [transform popup](C:/work/woby/artifacts/ui-review-2026-09-06/02-transform-popover.png). Source: [group controls and transform popup](C:/work/woby/src/main.cpp:916).

![Current transform editor](C:/work/woby/artifacts/ui-review-2026-09-06/02-transform-popover.png)

**4. Make exploratory editing recoverable**

Observed: after moving the sample from X=0 to X=1.444, Ctrl+Z did not restore the previous value. Reset worked, but resetting is not equivalent to restoring an arbitrary previous transform. Escape did not dismiss the transform popup in this session; clicking outside did. Source inspection found no application undo implementation or Ctrl+S/Ctrl+O handlers in the current event handling.

Recommended: add undo/redo for transforms, appearance, membership, and scene-object changes. Coalesce a continuous drag into one undo action. Add standard Open, Save, Save As, Undo, and Redo commands with visible shortcut labels. There is currently one Save toolbar action; once a scene has a path, it saves directly to that path. Make saving a separate copy straightforward.

Acceptance check: a drag followed by Undo restores the exact prior state; Redo reapplies it. Editing a numeric field does not move the camera. Escape exits popups predictably. Removing something from the scene can be undone.

Source: [keyboard event handling](C:/work/woby/src/main.cpp:2761), [Save behavior](C:/work/woby/src/main.cpp:3210).

**5. Put A/B assignment where users need it**

Observed: “New comparison” created an empty comparison. To fill A, I had to right-click the source, open **Comparison membership**, choose **Comparison 1**, and choose **Add to A**. B required the equivalent sequence. The inspector itself provides Clear controls for empty groups but no direct Add buttons. The README documents a faster two-object selection path, but this does not remove the friction from editing an existing comparison.

Recommended: make two explicit input sections: **A · Reference** and **B · Compared model**. Each should support **Add selected**, a source picker, and drag from the tree. Show names and part counts, preserve role labels through swaps, and replace the generic incomplete paragraph with “Add a model to B” when that is the actual missing step. Use a two-way direction selector such as **B → A** / **A → B**, with source names, instead of relying on the “Measure on A” checkbox.

Acceptance check: with models already loaded, a user can populate both sides without a context menu and can immediately tell which surface is measured against which.

Evidence: [empty comparison](C:/work/woby/artifacts/ui-review-2026-09-06/03-empty-comparison.png), [nested assignment menu](C:/work/woby/artifacts/ui-review-2026-09-06/04-comparison-membership-menu.png). Source: [comparison contents](C:/work/woby/src/comparison_view.cpp:378).

**6. Make heatmaps and exported images self-explanatory**

Observed: surface distance and measurement-direction switching worked. The panel explains gray and yellow/orange in text, but provides no continuous numeric legend. In the B → A sample, the reported maximum was about 0.833 while Color maximum was 0.5; there was no prominent indication that values exceeded the displayed color range. The exported PNG contains geometry without a legend, units, tolerance, direction, or comparison name.

Recommended: add a labeled color bar with model units, the tolerance threshold, numeric ticks, and a “≥ maximum” saturation indicator. Keep “Fit color range” nearby. Show sampled maximum, mean, P95, and area above tolerance as readable result summaries. Preserve the existing explanation that the measurements are approximate and unsigned. Offer export options for legend, comparison name, source roles, direction, tolerance, resolution, and scene/result scope.

Acceptance check: someone receiving the exported image can identify what was compared, which direction was measured, what the colors mean, and the units/threshold without reopening Woby. Do not infer millimeters from an OBJ/STL file; retain “model units” unless the user supplies a unit label.

Evidence: [heatmap and statistics](C:/work/woby/artifacts/ui-review-2026-09-06/06-comparison-heatmap.png), [actual exported PNG](C:/work/woby/artifacts/ui-review-2026-09-06/08-exported-comparison.png). Source: [heatmap controls](C:/work/woby/src/comparison_view.cpp:419), [statistics](C:/work/woby/src/comparison_view.cpp:468).

**7. Establish a calmer visual hierarchy**

Observed: blue fills are used for section headings, buttons, selection, and toggles. Many values and instructions share the same small text treatment. The default imported meshes show solid surfaces, edges, and vertices; together with the ground grid this makes the geometry busy. Turning off edges and grid made the sample easier to read. Toasts appeared in the inspector's upper area, competing with its contents.

Recommended: use neutral surfaces and separators, a single accent for selection/primary actions, and distinct on/off/disabled treatments. Use a quiet solid-view default with clearly named **Solid**, **Edges**, and **Vertices** toggles and optional inspection presets. Provide a selection outline and a consistent relationship between the selected tree row and geometry. Keep recurring instructional paragraphs in contextual help, while retaining short measurement limitations near results. Place save/load/export feedback in a reserved, readable location, with an Open folder action where useful.

A useful layout direction is a compact document/action bar at the top, an object tree on the left, the unobstructed canvas in the center, a selection/comparison inspector on the right, and navigation/status information at the bottom. The existing panel visibility controls can remain as shortcuts.

**8. Improve inspection continuity**

Observed: the diagnostic “First” button moved the camera to a boundary edge, but I had no visible next/previous sequence or clear identification of the focused edge. Saving and reopening preserved the sample's comparison inputs and appearance settings, but reframed the camera instead of restoring the framing used for the saved/exported review.

Recommended: provide diagnostic previous/next navigation, “1 of N”, a distinct highlight for the current edge, and an explicit A/B target. Consider saved camera views or persisting the last review view so inspection and exports can be reproduced. Add Frame selection and standard orientation views alongside Frame scene/result.

Evidence: [diagnostic focus](C:/work/woby/artifacts/ui-review-2026-09-06/07-diagnostic-focus.png), [reopened scene](C:/work/woby/artifacts/ui-review-2026-09-06/09-reopened-scene.png).

**Accessibility follow-up**

Windows UI Automation exposed the Woby window and native title-bar controls, but did not expose the in-app scene tree or controls in the captured accessibility tree. This warrants a separate screen-reader and keyboard-navigation review. Add meaningful control names/roles and a visible focus path. Do not treat the current absence of an automation tree as a completed accessibility test.

**What already works well**

Model import, visibility, render toggles, transform/reset, orbiting, R to reframe, Ctrl+B, automatic comparison calculation, measurement-direction switching, overlay mode, scene save/open, and PNG export all worked during the tested flows. The hierarchy and per-object settings are useful capabilities worth retaining. Tooltips and the explicit “model units”/approximate-distance explanations provide useful guidance once found.

**Validation and artifacts**

The existing Debug build passed `cmake --build --preset vs2026-vcpkg` with no warnings in its output. All 193 tests passed with `ctest --preset vs2026-vcpkg` in 102.55 seconds. No application source was changed and no new tests were added for this review. The test run and build used the existing configured preset; this was not a clean rebuild.

Files: [saved review scene](C:/work/woby/artifacts/ui-review-2026-09-06/review-scene.woby), [build log](C:/work/woby/artifacts/ui-review-2026-09-06/build-debug.log), [test log](C:/work/woby/artifacts/ui-review-2026-09-06/ctest.log). The review session was closed after saving.

For implementation, retain the project's state boundaries: scene-visible controls belong in UiState, edits go through ui_operations, and persisted scene properties need .woby mappings. Renderer/window/layout integration belongs in the runtime layer. Begin with viewport layout and primary actions, then the inspector/A-B flow, while treating undo and informative exports as explicit follow-up work.
