# Scene lifecycle contract

CTL scene commands run through the existing main-thread FIFO and request-key ledger.
They never open native dialogs or confirmation popups. Camera and pane state remain
session-only; camera navigation does not make the scene dirty.

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

## Validation

Run the normal unit suite with `ctest --preset vs2026-vcpkg`. With a desktop session,
run `python tests/ctl_lifecycle_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe` for
real CLI-to-viewer coverage, including screenshots after successful and rejected opens.
The smoke test starts its own viewer and exits it; its scene/model files are temporary.
GPU allocation failure cleanup is checked in the runtime, but is not fault-injected by
unit tests because GPU creation depends on the renderer runtime.
