# CTL functionality roadmap

Original investigation: 2026-09-05. Status updated after commit `e4985d1`.

## Current status and decisions

The original report is preserved below, with source links made relative to this
repository. Its descriptions of the initial implementation, proposed command names,
and recommendations reflect the time of that investigation. They are not a list of
currently supported commands; see [the automation API](automation.md) for that.

| Item | Current status |
| --- | --- |
| Typed commands processed on the main thread | Implemented in `a38788a`. HTTP workers do not access scene state or GPU resources. |
| Reliable object identifiers | Implemented in `cbd6af4`, including `objects.list` / `object.get` and the corresponding CLI commands. Inspection currently returns basic identity information, not all editable settings or the scene hierarchy. |
| Command ordering | Implemented in `e4985d1`: an eight-command FIFO queue, admission sequence IDs, timeout handling, and ownership of asynchronous screenshot completion. |
| Scene revisions | Implemented experimentally, then removed by decision. The per-frame snapshot cost was unnecessary for the intended workflow. The benchmark code and reports were also removed. |
| Automation concurrency contract | Assume one coordinating client and no manual edits during automation. Multiple clients and UI input must remain safe, but their effects may interleave; consistency across requests is the caller's responsibility. |
| Predictable command retries | Implemented: session-scoped command IDs, `command.get` / `ctl command`, request keys, retained results, explicit timeout states, and structured retry errors. Absolute setters and applied-value responses are requirements for future editing handlers. See [retry semantics](automation.md#recover-commands-and-retry-safely). |
| Persistence and destructive operations | Implemented: scene save/save-as/open/new and quit with explicit dirty policies, atomic saves, staged replacement, and existing request-key semantics. See [the lifecycle contract](scene-lifecycle.md). Camera remains session-only. |
| Remaining proposed functionality | Future work unless explicitly documented in the current API. |

The original revision-based capture and advanced-automation recommendations below
are superseded by the concurrency contract above. Keep command ordering and
main-thread execution; scene revisions are not a planned prerequisite.

## Original report

`ctl` could expose nearly every existing scene operation. The main work is adding command dispatch, object targeting, and completion handling; most scene-editing logic already exists.

I inspected the current working tree. Today, the API exposes only `instance.info` and `screenshot.capture`, with CLI discovery through `ctl instances`. The extension points are in [automation.cpp](../src/automation.cpp) and [ui_operations.h](../src/ui_operations.h).

**Functionality we can expose from existing features**

The command names below are proposals.

| Area | Potential commands | Implementation considerations |
|---|---|---|
| Instance status | `status`, `wait-ready`, `wait-idle`, `capabilities` | Report current scene, dirty state, loading/capture activity, version, and supported commands. Current `ready` indicates startup completion, not whether the app is busy. |
| Scene inspection | `scene info`, `scene tree`, `scene bounds` | Return hierarchy, file paths, importer IDs, settings, counts, and bounds as JSON. |
| Object inspection | `object get`, `object list` | Query folders, files, and groups, including local settings and effective inherited visibility/transforms. |
| Scene persistence | `scene open`, `scene save`, `scene save-as` | Existing load/save machinery; must handle unsaved changes and report completion without opening dialogs. |
| Model import | `model add`, `folder add`, `folder add --tree` | Reuse background loading, recursive discovery, and importer support. Return added/skipped/failed results. |
| Model removal | `model remove` | Existing behavior; must remove both logical state and GPU resources. |
| Visibility | `visibility set` | Target the whole scene, folder subtree, file, or group. |
| Render modes | `render set` | Independently enable solid mesh, triangle edges, and vertices at supported scopes. |
| Transforms | `transform get/set/reset` | Translation, rotation, and uniform scale for folders, files, and groups. |
| Appearance | `opacity set`, `color set/reset`, `vertex-size set` | Opacity at folder/file/group level; color at group level; vertex size globally and per file/group. |
| Scene helpers | `grid set`, `origin set`, `up-axis set` | Existing operations and scene persistence. Changing up-axis currently also reframes the camera. |
| Camera movement | `camera get`, `camera frame`, `camera orbit/pan/roll/dolly/move` | Existing camera math. Script commands should use explicit units rather than mouse-pixel conventions. |
| Importers | `importers list`, `importers add`, `importers scan` | Existing runtime functions; distinguish session registration from remembered configuration. |
| Diagnostics | `stats`, `performance get` | Expose mesh counts, renderer information, FPS, and available CPU/GPU timings through runtime snapshots. |
| App controls | `pane show/hide`, `pane width`, `quit` | Pane operations exist. Quit needs an explicit unsaved-change policy. |

Most editing commands can reuse [UiState](../src/ui_state.h) and its operation functions. File operations require more integration with the runtime code in [main.cpp](../src/main.cpp).

**Useful additions that require new behavior**

| Addition | What it enables | Scope |
|---|---|---|
| Absolute camera settings | Set target, orientation, distance, and field of view precisely | New validated operations over the existing camera struct |
| Standard camera views | Front/back/left/right/top/bottom/isometric | New presets respecting Y-up/Z-up; exact pole views need care with existing orbit limits |
| Frame a particular object | Focus on one file, group, or subtree | Compute target bounds; optionally distinguish visible geometry |
| Isolate objects | Show only specified objects, then restore visibility | Composite operations; restoration needs a saved visibility snapshot |
| Configurable screenshots | Width/height, helper overrides, background, overwrite policy | Screenshot dimensions and clear color are currently hardcoded |
| Repeatable capture | Apply settings and capture the corresponding scene revision | Explicit ordering and protection against intervening UI edits |
| Multi-view capture | Standard views, per-object thumbnails, turntable frame sequences | Camera control plus capture sequencing; video encoding would be separate |
| Scene clear/new | Reset the current scene | New operation coordinating state, GPU cleanup, scene path, and dirty tracking |
| Model reload/watch | Refresh changed source files | Define how settings and group identities survive geometry changes |
| Hierarchy editing | Create folders, rename, reparent, reorder, duplicate | New logical operations and transform/identity rules |
| Camera bookmarks | Save and restore named views | New state and persistence support |
| Jobs and progress | `jobs list/get/wait/cancel` | Generalize background work into identifiable operations |
| Script execution | Run command files, bulk edits, conditional workflows | Define sequencing, partial failure, and cancellation |
| Events | Observe scene changes, loading progress, capture completion | New subscription or cursor-based polling API |
| Picking and measurement | Query a vertex, inspect coordinates, measure distances | Some hover-picking machinery exists; stable object/vertex identification needs work |
| Window controls | Focus, resize, minimize, restore | New SDL runtime adapters |

Further possibilities include orthographic projection, clipping planes, lighting/material controls, annotations, mesh export, undo/redo, and headless rendering. These would be **new viewer capabilities**, with `ctl` providing their automation interface.

**The architectural work I would do first**

1. **Generalize the screenshot request slot into typed commands processed on the main thread.** HTTP handlers should continue to avoid accessing `UiState` or GPU resources directly. Keep structs and free functions.

2. **Introduce reliable object identifiers.** Files and groups currently use vector indices, and removal shifts them. Names and paths can also be ambiguous. Return opaque IDs from scene inspection, define their lifetime, and reject stale targets.

3. **Define completion precisely.** “Load complete” must include GPU finalization and scene commit. “Screenshot complete” already means the PNG was written. State edits should update bounds and dirty state before returning success.

4. **Add scene revisions and command ordering.** Scripts need confidence that “set camera → hide group → capture” produces the intended image. Main-thread execution alone does not prevent UI changes between separate requests.

5. **Make commands predictable to retry.** Prefer `visible=false` over toggles. Return applied values after clamping. Preserve structured error codes in CLI JSON, and give long operations IDs so a timeout does not require blindly repeating work.

6. **Specify persistence and destructive-operation behavior.** Open/new/quit should return an actionable dirty-scene error unless the caller supplies a save/discard policy. Camera state is currently explicitly excluded from `.woby` files; adding persistent camera controls requires updating that mapping and its tests.

There are also existing semantics to preserve or deliberately revise: file visibility updates its groups, transform reset also resets opacity, and model removal reframes the camera. These matter when commands become part of scripts.

**My recommended implementation order**

1. **Inspection foundation:** status, capabilities, scene tree, object IDs, object settings, camera state.
2. **Immediate capture value:** camera frame/set/presets, visibility, render modes, helpers, configurable screenshots.
3. **Complete workflows:** open/add/save/remove, progress, cancellation, and idle waiting.
4. **Full editing:** transforms, opacity, colors, point sizes, isolation.
5. **Advanced automation:** revisions, command sequences, events, reload/watch, multi-view capture.

The first useful milestone would let a script **discover the scene, target an object, frame it, configure its appearance, and capture it reliably**.

This was a source investigation; I made no changes and did not run builds or tests.
