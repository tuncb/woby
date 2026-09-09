# CTL command reference

Updated 2026-09-08. All commands in the current-command tables below are implemented
in the CLI and local JSON-RPC API. Comparison controls and measurements are available
alongside the existing scene controls, lifecycle, capture, and recovery
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
| `scene undo` | `scene.undo` | Undo one scene edit. Returns `action`, `applied`, and `dirty`; `applied: false` if no step is available. |
| `scene redo` | `scene.redo` | Redo one scene edit, with the same result fields as Undo. |
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

`scene undo` and `scene redo` share the UI's session history and take no operation
parameters. They accept the common timeout, request-key, wait, and JSON options.
If loading, GPU finalization, screenshot capture, a dialog, or an active widget edit
is present when a trigger executes, it returns `-32014` without consuming a history step.
Earlier CTL commands still finish first under the normal FIFO ordering. A failed
source restoration returns `-32004` with an `Undo skipped:` or `Redo skipped:`
message; it consumes that step and leaves the live scene unchanged. Retrying with
the same request key replays the original result/error without moving history again.
Each separately admitted trigger applies at most one step, including when several
commands run in the same frame. Camera movement remains outside history.

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
and mesh render-mode commands do not apply to comparison results. The commands
below create comparisons and edit their inputs and measurement settings.

Screenshot capture waits for every visible comparison to finish, including
single-input inspections. Empty comparisons, missing references, or failed
computations fail capture; hide or repair the affected object before retrying.

## Comparisons

| CLI | RPC method | Behavior |
| --- | --- | --- |
| `comparison create [--name TEXT] [--a OBJECT_ID] [--b OBJECT_ID]` | `comparison.create` | Create a comparison, optionally with an initial input on each side. Returns `target` (the new ID), `object`, `dirty`, and `bounds`. Omitted inputs leave that side empty. |
| `comparison delete COMPARISON_ID` | `comparison.delete` | Delete the comparison without deleting its source models. Returns `removed` and `dirty`. |
| `comparison set COMPARISON_ID [--name TEXT] [--visible BOOL] [--mode distance\|a\|b\|overlay\|surface_quality] [--distance-on-a BOOL] [--tolerance N] [--color-range N] [--show-edges BOOL] [--show-boundaries BOOL] [--show-non-manifold BOOL]` | `comparison.set` | Edit any supplied settings; at least one is required. Names must contain 1–511 UTF-8 bytes without NUL characters. |
| `comparison add COMPARISON_ID --side a\|b --object OBJECT_ID` | `comparison.add` | Add the input's current triangular parts to the selected side, deduplicating existing membership. |
| `comparison enable COMPARISON_ID --side a\|b --enabled BOOL [--object OBJECT_ID]` | `comparison.enable` | Enable or disable existing members on one side. Accepts a file, folder, or triangular mesh group; omit `--object` to change the whole side. Membership is preserved. |
| `comparison remove COMPARISON_ID --side a\|b --object OBJECT_ID` | `comparison.remove` | Remove the input's current triangular parts from the selected side. |
| `comparison clear COMPARISON_ID --side a\|b` | `comparison.clear` | Clear the entire side, including missing references. |
| `comparison swap COMPARISON_ID` | `comparison.swap` | Swap the A/B input lists. |
| `comparison results COMPARISON_ID` | `comparison.results` | Wait for fresh diagnostics and, with two inputs, both directed distance summaries. Works for hidden comparisons and in every display mode. |

Surface quality controls are also available through `comparison set`:
`--quality-metric longest_edge|equivalent_size|shape|size_jump`, `--quality-on-a BOOL`,
`--quality-minimum-enabled BOOL`, `--quality-maximum-enabled BOOL`,
`--quality-minimum-size N`, and `--quality-maximum-size N`. RPC parameter names
are `qualityMetric`, `qualityOnA`, `qualityMinimumEnabled`, `qualityMaximumEnabled`,
`qualityMinimumSize`, and `qualityMaximumSize`. Limits apply to longest edge in
model units; negative sizes clamp to zero and an enabled maximum is raised to an
enabled minimum when needed. Each populated `comparison results` side includes
`surfaceMeshQuality` with count/minimum/percentile5/median/percentile95/maximum for
all four metrics. Undefined metrics use null; degenerate faces are counted
separately. Surface quality mode works with either one or two inputs.

Use `objects`/`object` to discover comparisons and inspect their settings and inputs.
One populated side is sufficient for surface and edge inspection. Distance and
overlay modes require both sides; with one side the viewer displays that surface
and preserves the requested mode for when the second input is added.
Inputs are file, folder, or mesh group IDs; files/folders expand to current triangular
parts, just as in the UI. Add multiple inputs by repeating `comparison add` calls.
An input may belong to both sides or multiple comparisons. Empty/nontriangular inputs
and comparison IDs are rejected as inputs. All IDs are validated before edits, so a
bad B input does not leave a partially created comparison. Stale/foreign IDs fail with
`-32005`; malformed parameters or unsupported kinds use `-32602`.

Setters use the UI's normalization: tolerance is clamped to 0–1e12, color range to
1e-6–1e12 and at least the tolerance. Nonfinite numbers are rejected. All edits persist
in `.woby` scenes. Membership/settings setters return `target`, updated `object`,
`dirty`, and `bounds`. Use a request key when retrying creation or swapping.

`comparison enable` matches the comparison-view checkboxes. It only changes existing
members on the selected side of the selected comparison; source visibility and other
comparisons are unaffected. An input with no members on that side is a no-op.
`object COMPARISON_ID` and comparison edit responses include `enabled` on each
entry in `a` and `b`. RPC uses `comparison.enable` with `target`, `side`, boolean
`enabled`, and optional `object`.

```powershell
woby.exe ctl --instance review comparison enable COMPARISON_ID --side a --object OBJECT_ID --enabled false
woby.exe ctl --instance review comparison enable COMPARISON_ID --side a --object OBJECT_ID --enabled true
# Disable every member on side B.
woby.exe ctl --instance review comparison enable COMPARISON_ID --side b --enabled false
```

Measurements use an immutable snapshot of the transformed A/B geometry and tolerance
when the command starts. Ordinary source visibility and the comparison's display
offset do not affect distances. A background CPU calculation keeps the viewer responsive;
the command retains its FIFO slot until it finishes. Later CLI edits execute afterward.
Manual UI edits during computation do not change the snapshot. Empty comparisons
or missing references fail with `-32602`; loading/capture/dialog activity rejects
measurement with `-32014`.
Computation failures return a command error, never partial metrics.

Results include `target`, `tolerance`, `aToB`, and `bToA`. Each direction contains
`maximum`, `mean`, `percentile95`, `percentAboveTolerance` (0–100, strictly above
tolerance), `sampleCount`, `triangleCount`, and `diagnostics` counts (`boundaryEdges`,
`nonManifoldEdges`, `inconsistentWindingEdges`, `degenerateTriangles`, `duplicateTriangles`).
Distances are unsigned, in model units, using the same four centroid samples per
triangle as the UI. Mean, P95, and percentage are weighted by surface area; maximum
is the sample maximum, not an exact Hausdorff distance. Diagnostics describe the
source side of each direction.

For single-input inspection, the absent direction is `null`. The populated direction
contains triangle and diagnostic counts, `sampleCount: 0`, and `null` for
`maximum`, `mean`, `percentile95`, and `percentAboveTolerance` because no distance
measurement is available.

After a client timeout, an already running measurement continues and its result can
be recovered with `command` or the same request key. Reusing a measurement request key
returns its original snapshot; use a new key (or omit it) for fresh results.

For a running instance named `review`, with input IDs from `objects`:

```powershell
$comparison = woby.exe ctl --instance review comparison create --name "Repair check" --a A_ID --b B_ID --json | ConvertFrom-Json
woby.exe ctl --instance review comparison set $comparison.target --tolerance 0.01 --mode distance
woby.exe ctl --instance review comparison results $comparison.target --json
woby.exe ctl --instance review scene save-as C:\output\comparison.woby
```

Direct RPC uses `target` for the comparison ID, `a`/`b` for creation inputs, `object`
for an added/removed input, and camelCase settings such as `distanceOnA`, `colorRange`,
and `showNonManifold`. All commands use the existing authenticated `/rpc` endpoint.

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

