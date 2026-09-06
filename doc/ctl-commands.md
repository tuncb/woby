# CTL command reference

Updated 2026-09-06. All commands in the current-command tables below are implemented
in the CLI and local JSON-RPC API. The 34 commands exposing existing application
features are now available, alongside the earlier lifecycle, capture, and recovery
commands. See [automation.md](automation.md) for transport, ordering, retries, and
errors, and [ctl-roadmap.md](ctl-roadmap.md) for historical decisions.

## Invocation

Except for `woby ctl instances` and `woby ctl --help`, prefix each command with
`woby ctl --instance ID`. All scene controls accept `--json`, `--timeout SECONDS`
(1–3600; default 60), `--request-key KEY`, and `--wait`. Waiting is always enabled.
Use `woby ctl --help` or `capabilities` for discovery.

`TARGET` is `scene` where supported, or an opaque ID returned by `objects`.
`BOOL` means the literal `true` or `false`. `--tree` and `--remember` are flags.
Optional setter parameters preserve omitted values; setters with several optional
values require at least one. Vectors contain three finite numbers. CLI paths can
be relative; RPC paths must be absolute.

## Discovery, inspection, capture, and lifecycle

| CLI | RPC method | Result / behavior |
| --- | --- | --- |
| `woby ctl instances [--json]` | `instance.info` on discovered endpoints | Live instances, PID, URL, API version, startup readiness, queued count, active sequence. |
| `status` | `status` | Instance metadata, scene path/dirty state, loading/GPU finalization/capture/dialog busy state, version, and pane settings. |
| `capabilities` | `capabilities` | Supported methods, CLI usage, parameter types, scopes, limits, and importer formats. |
| `scene info` | `scene.info` | Scene path, dirty state, helper settings, up-axis, point size, mesh counts, render-mode counts, and bounds. |
| `scene tree` | `scene.tree` | Ordered hierarchy, object IDs, local settings, and effective settings per tree occurrence. |
| `scene bounds` | `scene.bounds` | Current visible-geometry bounds; default display bounds when no visible geometry remains. |
| `objects` | `objects.list` | Flat identity inventory, including hidden and unreferenced loaded objects. |
| `object OBJECT_ID` | `object.get` | Identity plus editable settings, counts, importer ID for files, local bounds where available, and effective values per tree occurrence. |
| `screenshot PATH` | `screenshot.capture` | Current camera/scene/helpers, no controls, 1920 × 1800 PNG. Creates parents and overwrites existing output; completes after GPU readback and PNG writing. |
| `scene save` | `scene.save` | Saves to the current scene path. |
| `scene save-as PATH [--overwrite]` | `scene.save-as` | Saves to a chosen path and adopts it as the current path. |
| `scene open PATH` | `scene.open` | Loads a `.woby` scene and replaces the scene after GPU preparation. |
| `scene new` | `scene.new` | Empty scene with no current path. |
| `quit` | `quit` | Accepts shutdown after applying the dirty policy. Observe process exit separately. |
| `command COMMAND_ID` or `command --request-key KEY` | `command.get` | Immediate command state and retained result/error lookup; accepts `--json`, but no timeout/wait. |

`scene open`, `scene new`, and `quit` accept `--on-dirty error|save|discard`
(default `error`). With `save`, use `--save-path PATH` when needed and `--overwrite`
to replace that explicit destination. CTL never opens confirmation dialogs.
See [scene-lifecycle.md](scene-lifecycle.md).

`status` and the other new inspection commands participate in the FIFO queue: they
observe state after earlier commands finish. They do not bypass an active CTL load
or provide job progress. `instance.info` and `command.get` remain immediate runtime
queries. `ready` means startup completed, not idle; a scene query does not wait for
unrelated manual background loading. `status.busy` describes loading, capture, and
dialog activity, not an idle barrier or the automation queue itself.

Comparison objects appear in `objects` and `scene tree` with kind `comparison`.
`object` includes their input references, missing-input status, and display
settings; `scene info` includes `comparisonCount`. Comparison IDs support
`visibility set`, `transform get`, translation-only `transform set`, and
`transform reset` (resetting their display offset). Rotation, scale, opacity,
and mesh render-mode commands do not apply to comparison results. Use the UI to
create comparisons and edit their inputs and measurement settings.

Screenshot capture waits for every visible comparison to finish. Incomplete
inputs or failed computations fail capture; hide or repair the affected object
before retrying.

## Visibility, rendering, transforms, and appearance

| CLI | RPC method | Scope / behavior |
| --- | --- | --- |
| `visibility set TARGET --visible BOOL` | `visibility.set` | Scene, folder subtree, file, group. File changes update all its groups; group changes refresh ancestor visibility. |
| `render set TARGET [--solid BOOL] [--triangles BOOL] [--vertices BOOL]` | `render.set` | Scene, folder subtree, file, group. Independent modes; file/folder controls apply to descendant groups. |
| `transform get OBJECT_ID` | `transform.get` | Local folder/file/group settings. |
| `transform set OBJECT_ID [--translation X Y Z] [--rotation-degrees X Y Z] [--scale S]` | `transform.set` | Local translation, Euler rotation, uniform scale; existing center/pivot and transform conventions. |
| `transform reset OBJECT_ID` | `transform.reset` | Resets translation, rotation, scale **and opacity** to their defaults, matching the UI. |
| `opacity set OBJECT_ID --value A` | `opacity.set` | Local folder/file/group opacity, clamped to 0–1. |
| `color set GROUP_ID --rgb R G B` | `color.set` | Group RGB, clamped to 0–1; preserves the existing color alpha. |
| `color reset GROUP_ID` | `color.reset` | Current default group palette color, matching UI reset. |
| `vertex-size set scene --pixels N` | `vertex-size.set` | Global base point size, clamped to 1–40 pixels. |
| `vertex-size set OBJECT_ID --scale S` | `vertex-size.set` | File/group multiplier, clamped to 0.1–10. No folder multiplier. |
| `grid set --visible BOOL` | `grid.set` | Ground-grid visibility. |
| `origin set --visible BOOL` | `origin.set` | Origin-axis visibility. |
| `up-axis set y|z` | `up-axis.set` | Scene up-axis; also reframes the camera. |

Object setters return `target`, `applied` local settings, `dirty`, and scene `bounds`.
Scene-wide setters return the updated scene snapshot. Scale is clamped to 0.01–20;
Euler rotation components to -180–180 degrees. Non-finite and out-of-float-range
numbers are rejected. Logical updates, bounds, and dirty tracking finish before
success is reported. These scene settings use the existing `.woby` mapping.

Local transforms use the reported `center` as pivot, the existing scale/rotation/
translation composition in [ui_state.cpp](../src/ui_state.cpp), and the renderer's
parent/local matrix multiplication. Effective transforms are returned as 16-number
`worldMatrix` arrays with translation in elements 12–14 (zero-based).

Tree entries contain `occurrence` index paths and `effective.visible`,
`effective.opacity`, and `effective.worldMatrix`. Repeated references share one
object ID and have separate occurrences. Object lookup returns these as an
`occurrences` array. Paths identify positions in that snapshot, not stable IDs.
`implicit` marks generated nodes when the renderer falls back to a default file
or group tree. Unreferenced objects have an empty occurrences array. Effective
opacity combines hierarchy opacity; color alpha remains a separate color setting.
File/folder render modes report enabled/total counts and on/off/mixed state.

## Camera and pane

| CLI | RPC method | Units / behavior |
| --- | --- | --- |
| `camera get` | `camera.get` | Target, eye, up vector, yaw/pitch/roll degrees, distance, vertical FOV, near/far planes, scene up-axis. |
| `camera frame` | `camera.frame` | Frame the whole scene using current bounds. |
| `camera orbit [--yaw-degrees Y] [--pitch-degrees P]` | `camera.orbit` | Relative changes to the stored camera angles, for either Y-up or Z-up. Pitch uses the existing ±1.45-radian limit. |
| `camera pan [--right R] [--up U]` | `camera.pan` | Move the target in rolled camera-local model units. |
| `camera roll --roll-degrees R` | `camera.roll` | Relative roll in degrees. |
| `camera dolly --factor F` | `camera.dolly` | Positive distance multiplier: 0.5 halves distance, 2 doubles it; minimum distance 0.001. |
| `camera move [--right R] [--up U] [--forward F]` | `camera.move` | Camera-local model units; positive forward moves toward the viewing direction. |
| `pane set [--visible BOOL] [--width N]` | `pane.set` | Viewer controls pane; width uses current layout units and window-dependent limits. |

Yaw and roll deltas are reduced modulo 360 degrees before application; the returned
angles show the applied values. Camera commands return `camera`; pane commands return `pane`. Camera
and pane state remain session-only and do not dirty the saved scene. Use request
keys for relative camera commands so a retry does not move the camera twice.

## Models, importers, and diagnostics

| CLI | RPC method | Behavior |
| --- | --- | --- |
| `model add PATH` | `model.add` | Append a model through background CPU loading and main-thread GPU finalization. |
| `folder add PATH [--tree]` | `folder.add` | Recursively discover supported model files; `--tree` retains the selected folder hierarchy. |
| `model remove FILE_ID` | `model.remove` | Remove logical data and GPU resources, prune empty folders, invalidate removed IDs, and reframe camera. |
| `importers list` | `importers.list` | Built-in extensions, loaded plugins with formats/versions/paths, and remembered registrations. |
| `importers add PATH [--remember]` | `importers.add` | Load an importer library; optionally register it for future launches. |
| `importers scan PATH [--remember]` | `importers.scan` | Discover and load importer libraries in a folder. |
| `importers forget PATH` | `importers.forget` | Forget a startup registration. The library remains loaded until exit. |
| `stats` | `stats` | Mesh/visibility counts, scene settings and bounds, renderer name, FPS. |
| `performance get` | `performance.get` | Last completed frame's stage/total/CPU timings and FPS. GPU timing is null when unavailable. |

Imports return `outcomes` per discovered/requested path, `addedIds` (file IDs),
`requestedCount`, `addedCount`, `failedCount`, `skippedCount`, `canceled`, and `dirty`.
Use `objects` or `scene tree` to discover the added groups/folders. An outcome has
`state` (`added`, `skipped`, `failed`, or `canceled`), its path, an added ID on success,
and an error for a failed input. Partial or all-input failures are successful batch
responses with failure counts: **check outcomes**, not just the CLI exit code.
Folder discovery filters unsupported extensions before loading, so they are not
included in requested/skipped counts. Empty folders succeed with zero additions.
Invalid folder paths or batch-level execution errors fail the command.

The FIFO slot remains owned through GPU commit, including after an HTTP timeout.
UI load cancellation is cooperative; when observed by the CPU loader, it reports a
canceled result and discards the uncommitted batch.
There is no CTL cancel command yet. Failed GPU files are reported alongside CPU
failures; successfully prepared files may still be added. Imports reframe the camera.

Importer add/scan return per-library `outcomes`, `loadedCount`, `failedCount`, and
`registrationError`. Loading can succeed even if saving a remembered registration
fails; results explicitly report both. These batch commands also require checking
outcomes. Omitting `--remember` leaves startup configuration unchanged. Importer
configuration belongs to the user profile, not the `.woby` scene.

Mutating scene/runtime commands reject a busy load, capture, or dialog with -32014.
Invalid parameters and unsupported target kinds use -32602; stale/foreign IDs use
-32005. New commands retain the existing FIFO, timeout, command-ID, and request-key
semantics. IDs survive ordinary edits/saves but not removal, scene replacement, or
viewer restart. No scene revision tracking or atomic multi-command transactions.

## RPC examples

CLI hyphenated options map to camelCase RPC parameter names. Positional object IDs
use `target` for edits; the existing `object.get` method continues to use `id`.

```json
{"jsonrpc":"2.0","id":1,"method":"render.set","params":{"target":"scene","solid":true,"triangles":false,"requestKey":"render-1"}}
{"jsonrpc":"2.0","id":2,"method":"transform.set","params":{"target":"OBJECT_ID","translation":[1,2,3],"rotationDegrees":[0,0,45],"scale":2}}
{"jsonrpc":"2.0","id":3,"method":"camera.move","params":{"forward":1,"requestKey":"move-1"}}
{"jsonrpc":"2.0","id":4,"method":"folder.add","params":{"path":"C:/models","tree":true,"timeoutSeconds":120,"requestKey":"load-1"}}
```

Replace `OBJECT_ID` with an ID from `objects.list`. Use fresh request keys for fresh
queries; replaying a keyed query intentionally returns its original snapshot.

## Additions requiring new behavior or contracts

These are separate from simply exposing existing controls. Syntax is proposed.

| Proposed commands / extensions | Required addition |
| --- | --- |
| `wait-ready`, `wait-idle` | A readiness wait can poll current metadata. A true idle wait must account for background loads, GPU finalization, captures, and admitted commands; an empty queue is insufficient. |
| `camera set --target X Y Z --yaw-degrees Y --pitch-degrees P --roll-degrees R --distance D --fov-degrees F` | Validated absolute camera operations for reproducible views; keep session-only unless persistence is explicitly added. |
| `camera view front|back|left|right|top|bottom|isometric` | Standard presets respecting Y-up/Z-up, including exact pole views beyond the current orbit pitch clamp. |
| `object bounds OBJECT_ID`, `camera frame --object OBJECT_ID [--visible-only]` | Compute transformed target/subtree bounds, define repeated-reference behavior, and optionally filter visible geometry. |
| `visibility isolate OBJECT_ID...`, `visibility restore TOKEN` | Composite visibility changes and a retained restore snapshot with stale-object handling. |
| `screenshot PATH --width W --height H --background ... --grid BOOL --origin BOOL --overwrite` | Configurable render targets and per-capture overrides. Define overwrite/default compatibility with today's unconditional overwrite and fixed size. |
| `capture views`, `capture turntable` | Sequence camera changes and screenshots, report outputs, and define whether/how to restore camera state. Video encoding is separate. |
| `load status`, `load cancel`, or generalized `jobs list/get/wait/cancel` | The UI already has load progress/cancel callbacks; CTL needs identifiable work, progress snapshots, cancellation boundaries, and terminal outcomes. Recovery via `command` alone does not provide these. |
| `run SCRIPT`, bulk setters | Client-side sequential scripts can reuse existing FIFO/retry semantics. Server-side bulk execution needs explicit ordering, partial-failure, and cancellation rules; JSON-RPC batches are currently rejected. |
| `events poll --cursor CURSOR` / event subscriptions | Event retention, cursor lifetimes, progress/change notification contracts. |
| `model reload FILE_ID`, `model watch FILE_ID` | Reload workflow and settings/group-identity preservation across geometry changes. |
| `folder create`, `object rename/reparent/reorder/duplicate` | New hierarchy operations, fresh IDs for copies, transform preservation rules, and persistence mapping. |
| `camera bookmark save/list/apply/delete` | Named view storage; persistent bookmarks require new logical state and `.woby` mapping. |
| `pick`, `vertex get`, `measure distance` | Reuse hover-picking foundations, but define coordinate spaces, stable geometry references, and behavior after reload. |
| `window focus/resize/minimize/restore` | New SDL runtime adapters and platform behavior. |

Orthographic projection, clipping planes, lighting/material editing, annotations,
mesh export, undo/redo, and headless rendering would be new viewer capabilities,
with CTL commands added alongside their implementation.

