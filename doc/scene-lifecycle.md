# Scene lifecycle contract

CTL scene commands run through the existing main-thread FIFO and request-key ledger.
They never open native dialogs or confirmation popups. The current camera is persisted on Save/Open, while pane state remains
session-only; ordinary camera navigation does not make the scene dirty.

## Commands

| RPC | CLI | Parameters beyond timeoutSeconds/requestKey |
| --- | --- | --- |
| `scene.save` | `scene save` | None; saves the current path, including overwriting it |
| `scene.save-as` | `scene save-as PATH` | `path`, optional `overwrite` (false) |
| `scene.open` | `scene open PATH` | `path`, optional `onDirty`, `savePath`, `overwrite` |
| `scene.new` | `scene new` | Optional `onDirty`, `savePath`, `overwrite` |
| `quit` | `quit` | Optional `onDirty`, `savePath`, `overwrite` |

RPC paths must be absolute filenames. CLI paths resolve against the caller's working
directory. Save paths receive the usual `.woby` extension when absent. Returned paths
are absolute and normalized. `overwrite` applies to an explicit save destination;
it is required to replace an existing file even when save-as names the current file.

`onDirty` is `error` (default), `save`, or `discard`. Policies are checked at execution,
against the current persisted-document baseline. On a dirty scene, `error` returns
`dirty_scene`; `save` saves the current path or explicit `savePath` before proceeding;
`discard` permits replacement or shutdown. An untitled dirty scene with `save` requires
`savePath`. On a clean scene the policy does not cause a save. `savePath` requires
`onDirty: "save"`; `overwrite` is only valid with an explicit save destination.

Busy background loading, GPU finalization, screenshot work, or open dialogs cause
`scene_busy` before saving or discarding anything. An open holds the FIFO slot through
CPU loading, GPU finalization, and logical scene commit. A client timeout/disconnect
does not cancel started work. UI cancellation of loading reports `scene_canceled`.
The existing single-coordinator/no-manual-edits automation contract applies.

## Failure and completion

Saving serializes the document using the final destination for relative model paths,
writes and closes a temporary file on the same filesystem, then atomically installs
it. No-overwrite installation must not race with destination creation. Failure leaves
the previous destination, current scene path, and clean baseline intact. This is
atomic replacement, not a guarantee of survival across power loss.

Opening prepares the complete logical state and GPU resources before swapping them
into the live scene. Parse/import/hierarchy/GPU failure or cancellation preserves the
current scene, path, camera, IDs, and baseline. A successful preliminary save remains
saved even if opening subsequently fails. Partial scene opens are not successful.

New produces an empty, clean, untitled scene with default scene settings and camera.
Pane preferences, running state, and the monotonically increasing object-ID allocator
survive replacement. Open/new invalidate all previous object IDs; save/save-as preserve
them. No lifecycle operation deletes source model files.

Save/open/new results contain `path` (null for untitled), `dirty`, and normal command
metadata. Quit also returns `quitAccepted: true`, meaning shutdown was accepted, not
that process exit has been observed. Later queued commands do not execute. Successful
quit responses are allowed to drain during server shutdown; delivery cannot be
guaranteed after a lost connection. History is session-only and vanishes on exit.

Errors include a stable `error.data.reason`, `path` (current scene path or null),
`dirty`, and normal command metadata. Reasons/codes are `dirty_scene` (-32011),
`save_path_required` (-32012), `destination_exists` (-32013), `scene_busy` (-32014),
`save_failed` (-32015), `open_failed` (-32016), and `scene_canceled` (-32017).
Invalid parameters use -32602 before admission.

Request keys cover the method, normalized paths, dirty policy, and overwrite policy.
Retries replay the original result or error without re-saving, reopening, clearing,
or quitting again. After a retained failure, use a new key for a changed intention.
After viewer exit/restart, reconcile files and instance identity before retrying.

## Scene edit history

The viewer records logical scene edits from UI controls and committed CTL changes.
Editing operations notify history by advancing `UiState::sceneEditRevision` via
`notifySceneEdit` (also called by `markSceneDirty`), even when the document is already
dirty. Frame/action boundaries flush those notifications; idle frames with unchanged
revisions do no document construction, comparison or snapshot allocation.
Struct-level setters must be followed by a
notification from their owning scene operation. Camera and other session-only
operations do not notify. Dirty-indicator synchronization does not notify either.
Notified edits are still compared with the current snapshot to discard no-ops;
snapshots retain complete metadata, and changed drag frames still update it.
Undo/Redo buttons and Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z restore the previous/next scene
snapshot. CTL `scene undo` / `scene redo` (RPC `scene.undo` / `scene.redo`) trigger
the same restoration path, one step per command, with retry-key deduplication.
A widget's active interaction, including its release frame, forms one
action. Saving closes the current action and retains history; dirty state always
compares restored content with the latest successfully saved document.

History has no action-count limit and retains the session's edits. Snapshots contain
source paths, importer IDs, part settings and object identities, with no vertices,
triangle indices or other mesh buffers. Removing a model releases its geometry.
No history is written to `.woby` files. New/Open clears history only after successful replacement;
a failed or canceled replacement leaves it intact. A new edit after undo discards
the redo branch. Camera, selection, pane/export preferences, and the monotonic object
ID allocator are not rewound by ordinary edits. Explicit Apply View actions restore
their before/after camera and selection; saved view contents participate in scene
history and dirty tracking. See [Saved views](views.md).
Restored objects keep their original IDs, including
comparison references and folder hierarchy.

Restoration reuses live geometry by object ID. Absent models are reloaded from their
source paths with their original importers. Changed geometry is accepted if the
part count, names and order still match; geometric centers and bounds are refreshed.
Logical state and missing GPU meshes are prepared before committing. Existing GPU
meshes are reused; removed resources are destroyed after successful preparation.
Sources are never written. Missing/unreadable sources, unavailable importers,
incompatible parts or GPU preparation failures leave the live scene unchanged and
show an error. The failed Undo/Redo action is consumed: the cursor advances and its
snapshot is rebased to the unchanged scene, so the next frame cannot invent an edit
or erase the redo branch. Later Undo/Redo commands proceed from that position.
Dirty tracking compares saved scene metadata, not the contents of external models.
History shortcuts are unavailable during active widget edits, modal/native dialogs,
file processing, or screenshot capture; nonmodal property popups allow them after
the active edit finishes. Text fields retain their own text undo.

## Validation

Run the normal unit suite with `ctest --preset vs2026-vcpkg`. With a desktop session,
run `python tests/ctl_lifecycle_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe` for
real CLI-to-viewer coverage, including screenshots after successful and rejected opens.
The smoke test starts its own viewer and exits it; its scene/model files are temporary.
Run `python tests/ctl_history_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe` for
Undo/Redo triggers, no-op boundaries, retries, camera exclusion, save/open/new,
branching, source reloads, and consumed restoration failures in a real viewer.
GPU allocation failure cleanup is checked in the runtime, but is not fault-injected by
unit tests because GPU creation depends on the renderer runtime.
