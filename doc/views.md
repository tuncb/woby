# Saved views

Views are named checkpoints of the current scene presentation. The Views section
in the left pane has a + button to capture the current state. Click a row to apply
its checkpoint, double-click or use F2/context-menu Rename to rename it, use Save
on the row to replace its checkpoint, and use Delete to remove the view. Row Save
updates the document in memory; Save scene / Save scene as writes the views to disk.

A view stores camera, up axis, grid/origin/dimensions toggles, vertex sizes, object
visibility, colors, opacity, render modes, local transforms, analysis display
settings/result positions, existing analysis input enable flags, and selection
in click order. Selection is captured because dimension annotations depend on it.
Geometry, source paths, object names, hierarchy, analysis membership, pane/layout
preferences, dialogs, and renderer caches are not captured. New objects absent
from a checkpoint retain their settings; parent transforms still affect descendants.

UiState owns lightweight UiView records. UI adapters call the view operations in
ui_operations.h; capture/application/persistence mapping live in
ui_view_operations.cpp. View identities and active-row state are session-only.
.woby version 7 stores [[views]], [views.camera], [[views.objects]], and
[[views.parts]]. Older scene versions load with no views. References use the
current document's file/group, flattened node, and analysis indices; Open maps
these to newly allocated runtime IDs. Object names are never used as unique keys.
Missing or duplicate checkpoint references are dropped at load. Removing files,
pruned folders, or analyses removes their references from every view. Applying
a view also prunes stale references. Views themselves remain, even when empty.
Open still fails for missing model files required by the scene itself.

Create/update/rename/delete and reference cleanup participate in dirty tracking
and undo. Cameras inside named views are document content, unlike the live camera.
Apply View is a single history action; its before/after camera and selection are
stored on that history transition. Undo/Redo restores navigation only when crossing
an Apply View action. Ordinary camera navigation remains outside history and dirty
tracking. Applying a camera/selection-only view creates an undo action without
making the document dirty. Appearance changes use the normal saved-document dirty
analysis. Undoing deletion restores the views and their object references together.

Unit coverage lives in tests/ui_view_tests.cpp. Validate with a Debug build using
`cmake --build --preset vs2026-vcpkg` and `ctest --preset vs2026-vcpkg`.
