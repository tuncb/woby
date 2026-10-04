# CTL command reference

Updated 2026-09-12. All commands in the current-command tables below are implemented
in the CLI and local JSON-RPC API. Analysis controls and measurements are available
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
values require at least one. Vectors contain three finite numbers, except annotation
`start`, `end`, and `delta`, which contain two. CLI paths can
be relative; RPC paths must be absolute.

## Surface annotations

These commands use runtime IDs from `objects` or `annotation list`. Creation takes
a **model group ID**; other annotation commands take an **annotation ID**. Explicit
IDs let scripts edit annotations without changing the current UI selection.
Creation includes sibling parts from the target's model file. If a selected parent
folder contains the target, its descendant parts define the scope instead. Unrelated
selected parts remain outside the scope.

| CLI | RPC method | Behavior |
| --- | --- | --- |
| `annotation list` | `annotation.list` | Return `annotations`, including properties, source IDs/status, and vertex coordinates. |
| `annotation get ANNOTATION_ID` | `annotation.get` | Return one annotation under `object`. |
| `annotation create GROUP_ID --shape line\|rectangle --start U V --end U V [--aspect N]` | `annotation.create` | Project the outline onto the source part and its visible siblings using the current camera. Returns its ID as `target` and its details as `object`. Accepts the same optional style fields as `annotation set`. |
| `annotation set ANNOTATION_ID [--name TEXT] [--comments TEXT] [--visible BOOL] [--locked BOOL] [--rgb R G B] [--opacity N] [--width N]` | `annotation.set` | Update only supplied properties. Empty comments clear the text. |
| `annotation reshape ANNOTATION_ID [--start U V] [--end U V]` | `annotation.reshape` | Change endpoints or opposite rectangle corners in the original drawing projection. At least one is required. |
| `annotation move ANNOTATION_ID --delta U V` | `annotation.move` | Translate both controls in the original drawing projection, preserving their separation. |
| `annotation delete ANNOTATION_ID` | `annotation.delete` | Remove the annotation; undo can restore it. |

**Drawing coordinates:** U and V range from -1 to +1. The drawing area's center is
`0 0`, U increases rightward, and V increases upward. Creation uses the current
camera pose and FOV with a **square authoring area by default**, independent of the
window and panels. `--aspect` specifies width/height (0.1–10) when a rectangular
authoring area is wanted. The chosen projection is frozen in the annotation.
Move and reshape always use that original projection, even after camera changes.
These values are not model-space distances or screen pixels.

`vertices` contains two line endpoints or four rectangle corners. `vertexSpace` is
`model` for single-part annotations and `world` for annotations with multiple source
parts. Coordinates use model units. The response also contains `start`, `end`, `projector`
(column-major model-to-clip matrix), `controlSpace`, `vertexSpace`, `sourceId`,
`sourceName`, `targetValid`, `effectiveVisible`, and `settings`. `settings.color`
is RGBA. `sourceIds` lists every source part, with null entries for missing sources;
`sourceId` retains the primary source for compatibility. Unresolved annotations
return an empty vertex array while retaining their comments and settings.

Creation accepts touching sibling surfaces without requiring shared mesh vertices,
and bridges empty gaps and depth transitions between surfaces within the target
model or group. Each straight 3D bridge stays attached to both surfaces; it may
become visible after rotating the view and stretches if its source parts move apart.
Rectangle corners at depth transitions use the frontmost attached surface for their
handles. Opaque occlusion by objects outside the group is still rejected. Move and
reshape require an unlocked annotation with all of its original source parts visible
and unchanged. Every endpoint or rectangle corner
must lie on the source surface in its original projection; edges may bridge holes.
Invalid edits leave the annotation unchanged. Names/comments/styles and deletion
remain available for locked or unresolved annotations. Width is clamped to 1–12
pixels, and RGB/opacity to 0–1. CLI comments accept up to 8192 UTF-8 bytes without
NUL characters, including newlines; names accept up to 511 bytes.

`object ANNOTATION_ID`, `scene tree`, `visibility set`, `color set/reset`,
`opacity set`, and `camera frame --object ANNOTATION_ID` also support annotations.
Generic transform/render-mode commands do not apply; use `annotation move` and
`annotation reshape`. Annotation edits use normal scene undo/redo and `.woby`
save/load. `scene info` includes `annotationCount`. Use a unique `--request-key`
for creation and relative moves when retries are possible.

```powershell
# Set $groupId to a visible model group ID from objects --json.
$created = woby.exe ctl --instance review annotation create $groupId --shape rectangle --start -0.1 -0.1 --end 0.1 0.1 --name "Inspect range" --request-key range-create-1 --json | ConvertFrom-Json
$annotationId = $created.target
woby.exe ctl --instance review annotation set $annotationId --comments "Check this surface" --rgb 1 0.5 0 --width 4
woby.exe ctl --instance review annotation move $annotationId --delta 0.05 0 --request-key range-move-1
woby.exe ctl --instance review annotation get $annotationId --json
woby.exe ctl --instance review scene save
```

For RPC, the positional ID is `target`, vectors are JSON arrays, and option names
match the table without `--` (for example `comments`, `locked`, `start`, `delta`).
The camera and model must be positioned so the requested drawing area hits the
intended surface before creation.

## Discovery, inspection, capture, and lifecycle

All scene/camera/capture commands also work in [headless mode](headless.md).
Launch with `woby --headless --instance ID`, poll `instances --json` for `ready`,
then use the same `ctl --instance ID` commands. The launch flag is not a CTL option.
`status` includes `headless`, `renderer`, and `screenshot` dimensions/format.
In headless mode `pane` is null and `pane set` returns `-32602`. `capabilities`
marks each method with `available`, which is false for `pane.set` in this mode.

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

Analysis objects appear in `objects` and `scene tree` with kind `analysis`.
`object` includes their input references, missing-input status, and display
settings; `scene info` includes `analysisCount`. Analysis IDs support
`visibility set`, `transform get`, translation-only `transform set`, and
`transform reset` (resetting their display offset). Rotation, scale, opacity,
and mesh render-mode commands do not apply to analysis results. The commands
below create analyses and edit their inputs and measurement settings.

Screenshot capture waits for every visible analysis to finish, including
single-input inspections. Empty analyses, missing references, or failed
computations fail capture; hide or repair the affected object before retrying.

## Analyses

Every detector has an **Automatic updates** checkbox in its gear menu. The Update
column provides play, stop, refresh, and retry actions. Automatic updates default
to on except for self-intersections; none is permanently automatic. Turning off
automatic updates retains completed findings. Relevant geometry/settings edits mark
manual results out of date and hide stale overlays until the next run. Cancellation
and failure wait for an explicit retry or a relevant geometry/settings change.

`analysis run ANALYSIS_ID --detector KEY` and `analysis cancel ANALYSIS_ID --detector KEY`
accept `boundary_edges`, `non_manifold_edges`, `inconsistently_oriented_tris`,
`non_manifold_vertices`, `holes`, `duplicate_points`, `duplicate_tris`,
`degenerate_tris`, and `self_intersections`.

`analysis set` exposes `--auto-update-boundaries`, `--auto-update-non-manifold`,
and `--auto-update-winding` for the three edge/orientation detectors. Existing
`--duplicate-points`, `--duplicate-triangles`, `--degenerate-triangles`,
`--non-manifold-vertices`, `--holes`, and `--auto-update-self-intersections`
control automatic scheduling for the other detectors. All accept a Boolean.
Manual run/cancel requests do not change these saved preferences.

| CLI | RPC method | Behavior |
| --- | --- | --- |
| `analysis create [--type mesh|surface_comparison|uv|uv_quality] [--name TEXT] [--a OBJECT_ID] [--b OBJECT_ID]` | `analysis.create` | Create an analysis, optionally with an initial input on each side. Returns `target` (the new ID), `object`, `dirty`, and `bounds`. Omitted inputs leave that side empty. |
| `analysis delete ANALYSIS_ID` | `analysis.delete` | Delete the analysis without deleting its source models. Returns `removed` and `dirty`. |
| `analysis set ANALYSIS_ID [--name TEXT] [--visible BOOL] [--mode distance\|a\|b\|overlay\|surface_quality] [--distance-on-a BOOL] [--tolerance N] [--color-range N] [--show-edges BOOL] [--show-boundaries BOOL] [--show-non-manifold BOOL] [--show-winding BOOL] [--topology-mode original_index\|exact_position]` | `analysis.set` | Edit any supplied settings; at least one is required. Names must contain 1–511 UTF-8 bytes without NUL characters. |
| `analysis add ANALYSIS_ID --side a\|b --object OBJECT_ID` | `analysis.add` | Add the input's current triangular parts to the selected side, deduplicating existing membership. |
| `analysis enable ANALYSIS_ID --side a\|b --enabled BOOL [--object OBJECT_ID] [--isolate BOOL]` | `analysis.enable` | Enable or disable existing members on one side. Accepts a file, folder, or triangular mesh group; omit `--object` to change the whole side. `--isolate true` requires an object and `--enabled true`; enables only its members on that side. Mesh supports A/B; UV supports A only. Membership is preserved. |
| `analysis remove ANALYSIS_ID --side a\|b --object OBJECT_ID` | `analysis.remove` | Remove the input's current triangular parts from the selected side. |
| `analysis clear ANALYSIS_ID --side a\|b` | `analysis.clear` | Clear the entire side, including missing references. |
| `analysis swap ANALYSIS_ID` | `analysis.swap` | Swap the A/B input lists. |
| `analysis results ANALYSIS_ID` | `analysis.results` | Wait for fresh diagnostics and, with two inputs, both directed distance summaries. Works for hidden analyses and in every display mode. |
| `analysis focus ANALYSIS_ID --side a\|b --detector KEY --index N` | `analysis.focus` | Focus and frame the one-based finding index on the selected side. Returns the selected `index`, full `count`, and camera state. Requires a visible analysis and current detector results. |

`analysis focus` accepts every detector key in `analysis.results`, including
holes and fins. The index is one-based and follows the order of each detector's
`findings` array. It can select findings beyond the first 100 returned by
`analysis results --json`, so use the returned `count` or the Diagnostics table
to see how many are available. Indexes refer to the current result and may
change after recomputation. For example:

```powershell
woby.exe ctl --instance review analysis focus ANALYSIS_ID --side a --detector holes --index 7 --json
woby.exe ctl --instance review analysis focus ANALYSIS_ID --side b --detector fins --index 2 --json
```

Exact source-duplicate controls are available through `analysis set`:
`--duplicate-points BOOL`, `--duplicate-triangles BOOL`,
`--show-duplicate-points BOOL`, and `--show-duplicate-triangles BOOL`.
The RPC names are `duplicatePoints`, `duplicateTriangles`, `showDuplicatePoints`,
and `showDuplicateTriangles`. The first two enable computation; Show changes only
presentation. These checks have no tolerance parameter.

Topology inspection uses `--topology-mode original_index|exact_position`.
Original-index mode is the default and preserves source indices for OBJ and importer
vertex tables. Both modes inspect selected parts separately within each source file;
coincident points in different files are never joined. Original source point IDs
are partitioned by world transform when parts move independently. Exact-position
mode joins exactly equal double-precision world positions, normalizing signed zero,
without an epsilon. Analysis display offsets do not affect topology.

`--show-winding` and `--show-non-manifold` control independent overlays and do not
invalidate analysis. Changing topology mode recomputes only the topology stage;
distance, quality, duplicates, and degenerate findings remain cached. Surface-quality
neighbor metrics and legacy `diagnostics` fields retain their original geometric
welding definitions.

The additive `boundary_edges`, `non_manifold_edges`, and
`inconsistently_oriented_tris` detector results include status, requested and resolved
per-source topology modes, provenance, excluded collapsed-face counts, edge endpoints,
and incident source triangle/part references. Triangle and point IDs are one-based;
edge IDs are one-based within a source and the current topology configuration.
Boundary and non-manifold counts count edges. Winding counts count unique affected
triangle instances; `findingCount` counts conflict edges. Both incident faces are
reported, without asserting which face is erroneous. Same-direction edges and BFS
orientation-contradiction witnesses have separate flags. An orientation contradiction
means manifold flip constraints cannot all be satisfied; witness edges are not a
repair prescription. Nondegenerate duplicate faces remain in edge incidence.

Each JSON array is bounded to 100 entries, including findings, incident faces,
endpoint source-point references, affected faces, and source metadata. Full counts
and truncation flags accompany these arrays. Unavailable or partial detectors have
a null `count` and a separate `knownCount`. UI pages and the incident-face list allow
inspection of all retained findings. Scene version 10 persists topology mode and
independent visibility; versions 2-9 migrate the old visibility switch to both.

Non-manifold vertices and hole loops use the same source topology:

```powershell
woby ctl analysis set ANALYSIS_ID --non-manifold-vertices true --holes true --hole-size-ratio-tolerance 0.05 --show-non-manifold-vertices true --show-holes true
```

RPC fields are `nonManifoldVertices`, `holes`, `holeSizeRatioTolerance`,
`showNonManifoldVertices`, and `showHoles`. The dimensionless hole ratio defaults
to 0.05; negative values normalize to zero and values above 1 are allowed.
Non-finite RPC numbers are rejected. Scene/operation normalization replaces
non-finite ratios with the default. Scene version 11 persists these settings,
including saved views, and continues loading versions 2�10 with defaults.

`non_manifold_vertices` requires each vertex link to be one cycle (interior) or
one path (boundary). Vertices incident to non-manifold edges are excluded and
counted separately in `excludedNonManifoldEdgeVertices`. Unused points are not
reported. Each finding includes the world position, source point/part references,
incident triangle instances, and number of connected link components. Amber
crosses mark findings; selection frames and highlights the incident faces.

`holes` traces boundary regions within each edge-connected component, before any
fin splitting. A simple loop qualifies when its axis-aligned world bounding-box
diagonal divided by its component diagonal is **less than or equal to** the
threshold. Each loop is counted once. Larger openings remain boundary findings;
this size test does not distinguish an intentional opening from a defect. The
ratio is invariant under translation and uniform scaling, but not generally under
rotation. Analysis display offsets do not participate.

Hole findings include an ordered vertex/edge cycle, source references, component
ID, both diagonals, and the ratio. `boundaryRegions` also exposes larger loops,
open chains, and branched regions, with separate counts. Open/branched boundaries
and unavailable size ratios are never classified as holes. Loop and boundary IDs
are one-based within the current analysis side; vertex/edge/component IDs are
one-based within the source topology. Collapsed faces remain excluded and counted.

Both detectors expose `status`, nullable `count`, `knownCount`, bounded findings,
and truncation flags. Arrays are limited to 100 entries, including loop members
and incident faces. Each loop vertex includes one representative source point/part
reference, its total reference count, and a truncation flag. Not-checked, outdated, canceled, or failed detectors have null current counts and empty findings. Previous known counts remain separate.
Automatic-update preferences control scheduling; Show switches only affect presentation.
Editing the hole threshold invalidates the hole detector. Other completed detectors retain their results. Blue loops mark holes; the UI has paged vertex/loop
navigation and explains excluded boundaries. Reports include counts and thresholds.

Each populated side of `analysis results --json` includes `detectors` schema version
1, with `duplicate_points` and `duplicate_tris`. `count` counts extra source records
and is null until a current check completes, or for unavailable or partial results; `knownDuplicateCount`
reports findings from available sources. Completed results have status `complete`, `unavailable`, or `partial`.
Pending, stale, stopped, and failed checks use the detector lifecycle statuses listed above, with separate previous `knownCount` values.
Detector failures are returned as failed statuses; source or measurement failures fail the request. Source and generated triangle IDs are 1-based.
The response includes up to 100 groups per detector and 100 members per group, with
explicit `findingsTruncated` and `membersTruncated` flags; totals remain exact for
completed checks. The UI offers the complete paged group list and scrollable members.
Legacy `diagnostics.duplicateTriangles` remains the geometric-duplicate count.

When an importer supplies original-point IDs, `duplicate_points` counts distinct
identities at equal coordinates. Intentional copies with the same ID count once;
different IDs remain duplicates even across groups. Finding member IDs stay as
one-based vertex-table indices, using the first selected vertex per identity.
Display geometry includes all selected copies and their transforms. These IDs
do not change `duplicate_tris` or topology. Importers without IDs keep the existing
per-vertex behavior.

Degenerate triangles are independently selectable:

```powershell
woby ctl analysis set ANALYSIS_ID --degenerate-triangles true --show-degenerate-triangles true --needle-threshold-ratio 1000 --cap-min-angle-degrees 177.5
```

Replace `ANALYSIS_ID` with the analysis ID returned by `objects`. RPC fields are
`degenerateTriangles`, `showDegenerateTriangles`, `needleThresholdRatio`, and
`capMinAngleDegrees`. The ratio is dimensionless and normalized to at least 1;
the angle is in degrees and normalized to [90, 180]. Both comparisons are strict:
longest/shortest edge ratio **greater than** the needle threshold, or maximum angle
**greater than** the cap threshold. Collapsed/collinear triangles are always
included. Zero-length edges are handled before division and classified as collapsed.
No distance-heatmap tolerance is used.

`detectors.degenerate_tris` adds the union count, overlapping `reasonCounts`,
thresholds, algorithm revision, status, and up to 100 findings. Each finding names
the source, part ID, 1-based generated triangle ID, reason flags, edge ratio, and
maximum angle. Non-finite/unavailable measurements are JSON `null`.
`findingsTruncated` describes the bounded listing; totals remain exact for all
available sources. Incomplete, unavailable, or partial results have `count: null`;
`knownCount` reports the available subset or the previous completed count.

Detection uses retained source records transformed into world coordinates in
double precision, excluding the analysis display offset. Detail already lost in the source format
cannot be recovered. Counts refer to source triangle / transformed part instances;
overlapping selections do not multiply counts. Missing
source records report unavailable rather than zero defects. Legacy
`diagnostics.degenerateTriangles` and `surfaceMeshQuality.degenerateTriangles`
retain their numerical-collapse definitions.

In the Diagnostics table, click the gear button on the **Degenerate triangles**
row to edit thresholds in a small popup for that analysis. The settings remain
available while detection is off or computing. Select the row to page through findings. Arrows select and frame triangles; purple overlays show
findings and yellow highlights the focused triangle. Crosses mark collapsed faces.
Automatic updates and Show are independent. Threshold edits invalidate only this detector's
cached stage; Show and analysis display-offset edits do not rerun detection.
Scene version 9 persists these controls and loads versions 2–8 with default
thresholds and the detector enabled. Findings and focus are not saved.

Surface quality controls are also available through `analysis set`:
`--quality-metric longest_edge|equivalent_size|shape|size_jump`, `--quality-on-a BOOL`,
`--quality-minimum-enabled BOOL`, `--quality-maximum-enabled BOOL`,
`--quality-minimum-size N`, and `--quality-maximum-size N`. RPC parameter names
are `qualityMetric`, `qualityOnA`, `qualityMinimumEnabled`, `qualityMaximumEnabled`,
`qualityMinimumSize`, and `qualityMaximumSize`. Limits apply to longest edge in
model units; negative sizes clamp to zero and an enabled maximum is raised to an
enabled minimum when needed. Each populated `analysis results` side includes
`surfaceMeshQuality` with count/minimum/percentile5/median/percentile95/maximum for
all four metrics. Undefined metrics use null; degenerate faces are counted
separately. Surface quality mode works with either one or two inputs.

Use `objects`/`object` to discover analyses and inspect their settings and inputs.
One populated side is sufficient for surface and edge inspection. Distance and
overlay modes require both sides; with one side the viewer displays that surface
and preserves the requested mode for when the second input is added.
Inputs are file, folder, or mesh group IDs; files/folders expand to current triangular
parts, just as in the UI. Add multiple inputs by repeating `analysis add` calls.
An input may belong to both sides or multiple analyses. Empty/nontriangular inputs
and analysis IDs are rejected as inputs. All IDs are validated before edits, so a
bad B input does not leave a partially created analysis. Stale/foreign IDs fail with
`-32005`; malformed parameters or unsupported kinds use `-32602`.

Setters use the UI's normalization: tolerance is clamped to 0–1e12, color range to
1e-6–1e12 and at least the tolerance. Nonfinite numbers are rejected. All edits persist
in `.woby` scenes. Membership/settings setters return `target`, updated `object`,
`dirty`, and `bounds`. Use a request key when retrying creation or swapping.

`analysis enable` matches the analysis-view checkboxes. It only changes existing
members on the selected side of the selected analysis; source visibility and other
analyses are unaffected. An input with no members on that side is a no-op.
`object ANALYSIS_ID` and analysis edit responses include `enabled` on each
entry in `a` and `b`. RPC uses `analysis.enable` with `target`, `side`, boolean
`enabled`, and optional `object`.

Add `--isolate true` with `--object OBJECT_ID --enabled true` to enable only that
object's existing members on the selected side, disabling every other member there.
Files and folders include their descendant parts already on that side; isolation
never adds membership. Objects with no members on that side are rejected before
any changes. Mesh analyses support either side; UV analyses support A only. The
other side, other analyses, and source visibility are unchanged. This changes
analysis inputs and results, following the existing automatic/manual update settings.
RPC uses the optional boolean `isolate`. Enable the whole side to include all
members again; this does not restore a previous mixed enablement selection.

```powershell
woby.exe ctl --instance review analysis enable ANALYSIS_ID --side a --object OBJECT_ID --enabled false
woby.exe ctl --instance review analysis enable ANALYSIS_ID --side a --object OBJECT_ID --enabled true
# Disable every member on side B.
woby.exe ctl --instance review analysis enable ANALYSIS_ID --side b --enabled false
# Analyze only one object's members on side A.
woby.exe ctl --instance review analysis enable ANALYSIS_ID --side a --object OBJECT_ID --enabled true --isolate true
# Include every member on side A again.
woby.exe ctl --instance review analysis enable ANALYSIS_ID --side a --enabled true
```

Measurements reuse the viewer's caches for the current transformed A/B geometry and
capture tolerance when the command starts. Missing topology, enabled duplicate,
quality, and distance stages run in the shared background queue (at most two analyses
at once). Repeated queries reuse completed stages. Ordinary source visibility,
Show toggles, and the analysis's display offset do not invalidate these results.
The command retains its FIFO slot until it finishes; later CLI edits execute afterward.
If a manual UI edit changes inputs, transforms, automatic-update preferences, or the scene while the
request is pending, it returns an error asking for a retry rather than mixing revisions.
Empty analyses
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
$analysis = woby.exe ctl --instance review analysis create --name "Repair check" --a A_ID --b B_ID --json | ConvertFrom-Json
woby.exe ctl --instance review analysis set $analysis.target --tolerance 0.01 --mode distance
woby.exe ctl --instance review analysis results $analysis.target --json
woby.exe ctl --instance review scene save-as C:\output\analysis.woby
```

Direct RPC uses `target` for the analysis ID, `a`/`b` for creation inputs, `object`
for an added/removed input, and camelCase settings such as `distanceOnA`, `colorRange`,
and `showNonManifold`. All commands use the existing authenticated `/rpc` endpoint.

## UV analysis

`analysis create --type uv --a OBJECT_ID` creates an independent UV analysis next
to its source. Omit `--type` (or use `--type mesh`) for the existing mesh analysis.
UV analyses use source input A only; mesh detectors, B, and input swapping do not
apply. Existing scenes and commands keep their previous behavior.

```text
woby ctl --instance review analysis create --type uv --name "UV inspection" --a OBJECT_ID
woby ctl --instance review analysis set ANALYSIS_ID --uv-view layout --uv-density-u 8 --uv-density-v 12 --show-edges true
woby ctl --instance review analysis set ANALYSIS_ID --uv-view surface --uv-grid true
woby ctl --instance review transform set ANALYSIS_ID --translation 3 0 0
```

`layout` displays existing UV islands in the scene up-axis plane at one uniform display scale;
`surface` shows the grid on a 3D copy. Grid density remains cells per supplied UV
unit, independently clamped to 0.1-1000. Overlaps and tile offsets are preserved.
Parts without complete UVs are omitted from the layout and shaded normally in the
3D view. These controls do not change the source mesh appearance. RPC names are
`type`, `uvView`, `uvGrid`, `uvDensityU`, and `uvDensityV`. Settings, result position,
and source membership support scene persistence, saved Views, and Undo/Redo.

### Patch inspection, gradients, and UV quality

Both UV analysis types support `--uv-separated true` for a display-only layout of
independent patches, using one common scale. Original UVs remain unchanged. With
`--uv-linked-selection true` (default), selecting a rendered patch selects its source
and highlights both copies. The analysis membership context menu provides isolation.

```powershell
woby ctl --instance review analysis set ANALYSIS_ID --uv-view layout --uv-separated true
woby ctl --instance review analysis enable ANALYSIS_ID --side a --object PATCH_ID --enabled true --isolate true
woby ctl --instance review analysis enable ANALYSIS_ID --side a --enabled true
woby ctl --instance review analysis set ANALYSIS_ID --uv-color u --uv-minimum -2 --uv-maximum 3
woby ctl --instance review render set scene --uv-grid true --uv-color v --uv-minimum 0 --uv-maximum 1
woby ctl --instance review analysis create --type uv_quality --a OBJECT_ID --name "UV quality"
woby ctl --instance review analysis set QUALITY_ID --uv-metric area --uv-normalization per_patch
woby ctl --instance review analysis results QUALITY_ID
```

`uvColor` / `--uv-color` accepts `grid`, `u`, or `v`. `uvMinimum` and `uvMaximum`
define the blue-to-yellow parameter range; maximum must exceed minimum. Values
outside the range use endpoint colors. The existing `uvGrid` switch enables any
of these coloring modes, preserving older scenes and commands. Parent edits apply
to UV-bearing descendants, retaining individual child overrides in saved scenes.

`surface_comparison` creates a dedicated A/B surface comparison. The legacy
`mesh` type still accepts both input options. Switching to `surface_quality`
consolidates its inputs into one Sources list, retaining disabled parts and
using the enabled state if either input enabled a repeated part. Quality uses
input A internally; side B source commands act on the same list and swapping
has no effect.

`uv_quality` is a single-input analysis, initially showing the 3D surface.
The UV inspection UI can switch between Layout (`uv`) and Distortion (`uv_quality`)
while retaining inputs and display settings. Its finding list includes missing UVs
and has no 100-finding cutoff. Selecting a finding frames and highlights its triangle
without leaving the analysis inspector; missing or collapsed UVs open in 3D.
Previous/Next finding wraps through the list, and Full result clears the highlight.
Finding selection is temporary; analysis settings still support scenes, views, and undo.
It supports `uvView`, `uvSeparated`, `uvLinkedSelection`, and `showEdges`, but uses
metric heatmaps instead of grid or parameter colors. `uvMetric` accepts:

- `angle`: maximum absolute difference between corresponding 3D and UV corner
  angles, in degrees. Blue is zero, yellow is 45, red is 90 or greater.
- `area`: UV triangle area divided by world-space triangle area. With
  `uvNormalization=per_patch` (default), divide this ratio by the patch's total
  valid UV area / total valid surface area. `absolute` retains parameter/world
  units. Colors use signed log2: blue at 1/8, neutral at 1, red at 8, saturating beyond.
- `orientation`: blue for positive UV winding, red for negative. A uniformly
  mirrored patch is valid. Mixed signs within one imported patch are findings;
  independent overlapping domains are not classified as errors.
- `anisotropy`: maximum / minimum singular value of the surface-to-UV triangle
  map. Colors use log2: blue at 1, yellow at 8, red at 64 or more.
- `min_stretch`: minimum singular value, in UV units / surface unit in absolute
  mode; per-patch mode divides by sqrt(total valid UV / surface area). Blue is 1
  or more, yellow 0.1, red 0.01 or less. Smaller values approach UV collapse.
- `overlap`: enables overlap checks. Red marks within-patch positive-area overlap;
  yellow marks cross-patch-only overlap. Shared edges and vertices are excluded.

Additional `analysis set` flags (RPC uses the corresponding camelCase names):

```text
--uv-threshold-enabled true --uv-threshold 2
--uv-near-collapse 0.01
--uv-range-enabled true --uv-range-minimum 1 --uv-range-maximum 4
--uv-overlap-enabled true --uv-overlap-scope per_patch|selected_patches
```

Thresholds are strict: greater than the value, absolute log2 magnitude for area,
and less than the value for minimum stretch. Histogram ranges are inclusive and
take precedence over threshold highlighting. Thresholds must be nonnegative;
near-collapse thresholds must be positive. `selected_patches` explicitly treats
selected patches as sharing a domain and reports cross-patch pairs separately.
The default `per_patch` excludes independent domains. Checks run on the analysis
worker, bounded by 2,000,000 broad-phase candidates and 10,000 pairs;
`overlaps.truncated` means a partial result.

`uvQuality` includes `metric`, `convention=surface_to_uv`, `statistics` (minimum,
median, p95, maximum, thresholdCount, thresholdAreaPercent, nearCollapseCount),
`histogram` (bounds, counts, surfaceAreas), and `overlaps` (checked, truncated,
scope, candidateCount, affectedTriangles, crossPatchPairs, and pairs containing
first/second part IDs and one-based triangle numbers). Percentiles give valid
faces equal weight; area percentages use physical surface area. Invalid mappings
are excluded.


Every UV diagnostic can be configured and checked through `ctl`, including in
`--headless` instances. `analysis results` waits for the current measurements;
call it after edits before requesting triangle pages or setting a probe.

```powershell
# Signed UV-area / surface-area distortion (negative = compression).
woby ctl --instance review analysis set QUALITY_ID --uv-metric area --uv-normalization absolute
woby ctl --instance review analysis results QUALITY_ID --json
# Stretch anisotropy; statistics and histogram are in uvQuality.
woby ctl --instance review analysis set QUALITY_ID --uv-metric anisotropy --uv-threshold-enabled true --uv-threshold 3
# Inclusive histogram range; overrides threshold highlighting.
woby ctl --instance review analysis set QUALITY_ID --uv-range-enabled true --uv-range-minimum 10 --uv-range-maximum 20
# Minimum stretch and near-collapse classification.
woby ctl --instance review analysis set QUALITY_ID --uv-metric min_stretch --uv-near-collapse 0.01 --uv-range-enabled false
# Overlap inspection; selected_patches checks a shared UV domain.
woby ctl --instance review analysis set QUALITY_ID --uv-metric overlap --uv-overlap-scope selected_patches
woby ctl --instance review analysis results QUALITY_ID --json
# Bounded per-triangle values and flags; sourcePartId is a usable CLI object ID.
woby ctl --instance review analysis uv-triangles QUALITY_ID --offset 0 --limit 100 --json
# Linked triangle/point probe, using a sourcePartId and one-based triangle number.
woby ctl --instance review analysis uv-probe QUALITY_ID --object PART_ID --index 1 --barycentric 0.5 0.2 0.3 --json
woby ctl --instance review analysis uv-probe-get QUALITY_ID --json
woby ctl --instance review analysis uv-probe-clear QUALITY_ID --json
```

`analysis uv-triangles` returns `items`, `total`, `nextOffset`, and `revision`.
Offsets are zero-based; limits are 1-100. Pass the returned `--revision` on subsequent
pages to reject results changed by an edit or recomputation. Each record reports
all metrics (not just the selected metric), raw UV/surface areas, UV vertices,
`valid`, `missingUv`, `collapsedUv`, `degenerateSurface`, `mixedOrientation`,
`nearCollapse`, `thresholdExceeded`, `highlighted`, `overlapping`, and
`crossPatchOverlap`. Invalid metrics are null. Overlap flags are null until checked;
`overlapTruncated` means that unreported overlaps may still exist.
`statistics.highlightedCount` and `highlightedAreaPercent` describe the actual
threshold/range highlight, with `thresholdEnabled`, `rangeEnabled`, `rangeMinimum`,
and `rangeMaximum` recording its settings.

`analysis uv-probe` requires an enabled UV quality analysis with linked selection
and an enabled source part. It defaults to the triangle centroid. Barycentric
weights follow the imported triangle's vertex order, must be in [0,1], and sum
to 1. The response includes the triangle measurements, weights, interpolated
`uv`, `surfacePosition`, and `displayPosition` (including the analysis translation
and any layout/separation). Positions use scene coordinates; add `coordinateOrigin`
to `surfacePosition` to recover original world coordinates. Missing UVs return
null UV coordinates; a triangle omitted from layout has a null display position.
Probes are transient: they do not dirty the scene or persist in `.woby` files.
`uv-probe-get` returns a null `probe` when none is selected, or a null `probe` with
`stale=true` after its inputs change or linked selection is disabled.
`uv-probe-clear` works even when measurements are stale. Summary findings also
include `sourceObject`, and overlap pairs include `firstObject` / `secondObject`,
which can be passed directly to `--object`; existing numeric part IDs are retained.

The end-to-end CLI check exercises all six diagnostics, pagination, source/layout
probes, invalid inputs, and save/load, with rendered color checks:

```powershell
uv run tests/ctl_uv_quality_smoke.py build/vs2026-vcpkg/bin/Debug/woby.exe
```

Collapsed UV triangles are magenta; missing UVs and degenerate surface triangles
are gray. Missing-UV patches are omitted from layout. Collapsed UVs have no visible
area there, so inspect them in the surface view. Findings use one-based triangle
numbers within each source patch. `analysis results` adds `uvQuality` with counts,
angle/area ranges and up to 1000 findings (`findingCount`, `findingsTruncated`).
Isolate patches to narrow a large result. All controls and membership changes support
undo/redo, saved views and `.woby` round trips. Versions 2-18 remain readable.


## Visibility, rendering, transforms, and appearance

UV grid options use raw supplied UV units; density clamps to 0.1-1000 independently
for U and V. Enable solid rendering to see the grid. Scene/file/folder targets edit
only parts with complete UVs; a target with none returns `-32602` without applying
other render options. Omitted settings are preserved. Group `object` output includes
`hasTexcoords` and settings `uvGrid`, `uvDensityU`, and `uvDensityV` (also the RPC
parameter names). Scene saves, saved Views, Undo/Redo, and headless PNGs include
these settings. For example:

```text
woby ctl --instance review render set scene --solid true --uv-grid true --uv-density-u 8 --uv-density-v 12
```

| CLI | RPC method | Scope / behavior |
| --- | --- | --- |
| `visibility set TARGET --visible BOOL` | `visibility.set` | Scene, folder subtree, file, group, analysis, or annotation. File changes update all its groups; group changes refresh ancestor visibility. |
| `render set TARGET [--solid BOOL] [--triangles BOOL] [--vertices BOOL] [--uv-grid BOOL] [--uv-density-u N] [--uv-density-v N] [--line-width N] [--line-depth-test BOOL]` | `render.set` | Scene, folder subtree, file, group. Independent modes; file/folder controls apply to descendant groups. |
| `transform get OBJECT_ID` | `transform.get` | Local folder/file/group settings. |
| `transform set OBJECT_ID [--translation X Y Z] [--rotation-degrees X Y Z] [--scale S]` | `transform.set` | Local translation, Euler rotation, uniform scale; existing center/pivot and transform conventions. |
| `transform reset OBJECT_ID` | `transform.reset` | Resets translation, rotation, scale **and opacity** to their defaults, matching the UI. |
| `opacity set OBJECT_ID --value A` | `opacity.set` | Local folder/file/group or annotation opacity, clamped to 0–1. |
| `color set OBJECT_ID --rgb R G B` | `color.set` | File/folder descendant groups, individual group, or annotation RGB; clamped to 0–1; preserves the existing color alpha. |
| `color reset OBJECT_ID` | `color.reset` | Reset each file/folder descendant or individual group to its palette color, or an annotation to its default; preserves opacity. |
| `vertex-size set scene --pixels N` | `vertex-size.set` | Global base point size, clamped to 1–40 pixels. |
| `vertex-size set OBJECT_ID --scale S` | `vertex-size.set` | File/group multiplier, clamped to 0.1–10. No folder multiplier. |
| `grid set --visible BOOL` | `grid.set` | Ground-grid visibility. |
| `dimensions set --visible BOOL` | `dimensions.set` | Selected-geometry dimension visibility; saved and undoable. Values use raw coordinates. |
| `origin set --visible BOOL` | `origin.set` | Origin-axis visibility. |
| `up-axis set y|z` | `up-axis.set` | Scene up-axis; also reframes the camera. |

Imported line groups support `--line-width` (1-12 drawable pixels, default 2) and
`--line-depth-test` (default true; false draws on top). These options apply only
to line descendants and reject targets with none. Other render flags are preserved.
`object` and `scene tree` report each group's `primitive` as `triangles` or `lines`;
group details include `lineSegmentCount` and settings `lineWidth`/`lineDepthTest`.
File details and `stats` also include `lineSegmentCount`. Parent color edits include
hidden descendants; later child color overrides are preserved in scenes and views.

Object setters return `target`, `applied` local settings, `dirty`, and scene `bounds`.
Scene-wide setters return the updated scene snapshot. Scale is clamped to 0.01–20;
Euler rotation components to -180–180 degrees. Non-finite and out-of-float-range
numbers are rejected. Logical updates, bounds, and dirty tracking finish before
success is reported. These scene settings use the existing `.woby` mapping.

Local transforms use the reported `center` as pivot, the existing scale/rotation/
translation composition in [ui_state.cpp](../src/ui_state.cpp), and the renderer's
parent/local matrix multiplication. Effective transforms are returned as 16-number
`worldMatrix` arrays with translation in elements 12–14 (zero-based).

Tree entries contain `parentId` (null at the root), `occurrence` index paths and `effective.visible`,
`effective.opacity`, and `effective.worldMatrix`. Repeated references share one
object ID and have separate occurrences. Object lookup returns these as an
`occurrences` array, including each occurrence's `parentId`. Paths identify positions in that snapshot, not stable IDs.
`implicit` marks generated nodes when the renderer falls back to a default file
or group tree. Unreferenced objects have an empty occurrences array. Effective
opacity combines hierarchy opacity; color alpha remains a separate color setting.
File/folder render modes report enabled/total counts and on/off/mixed state.

## Camera and pane

| CLI | RPC method | Units / behavior |
| --- | --- | --- |
| `camera get` | `camera.get` | Target, eye, up vector, yaw/pitch/roll degrees, distance, vertical FOV, near/far planes, scene up-axis. |
| `camera set [--target X Y Z] [--yaw-degrees Y] [--pitch-degrees P] [--roll-degrees R] [--distance D] [--fov-degrees F] [--near-plane N]` | `camera.set` | Absolute camera values; at least one required. Omitted fields are preserved, subject to normalization below. |
| `camera look-at --eye X Y Z --target X Y Z` | `camera.look-at` | World-space eye and target. Derives yaw, pitch, and distance; preserves roll and FOV. |
| `camera frame [--object OBJECT_ID]` | `camera.frame` | Fit the whole scene, or the union of visible, transformed occurrences/descendants of a file, folder, group, analysis, or annotation. Preserve orientation and FOV. Empty/hidden targets fail without moving the camera. Does not change selection. |
| `camera orbit [--yaw-degrees Y] [--pitch-degrees P]` | `camera.orbit` | Relative changes to the stored camera angles, for either Y-up or Z-up. Pitch is limited to ±90 degrees, including exact poles. |
| `camera pan [--right R] [--up U]` | `camera.pan` | Move the target in rolled camera-local model units. |
| `camera roll --roll-degrees R` | `camera.roll` | Relative roll in degrees. |
| `camera dolly --factor F` | `camera.dolly` | Positive distance multiplier: 0.5 halves distance, 2 doubles it; minimum distance 0.001. |
| `camera move [--right R] [--up U] [--forward F]` | `camera.move` | Camera-local model units; positive forward moves toward the viewing direction. |
| `pane set [--visible BOOL] [--width N]` | `pane.set` | Viewer controls pane; width uses current layout units and window-dependent limits. |

Yaw and roll deltas are reduced modulo 360 degrees before application; the returned
angles show the applied values. Camera commands return `camera`; pane commands return `pane`. Camera
navigation and pane edits do not dirty the scene. Explicit scene saves include the
current camera; saved views also capture it. Use request
keys for relative camera commands so a retry does not move the camera twice.

`camera.set` and `camera.look-at` use the existing logical camera and `.woby` mapping.
RPC parameters use `target`/`eye` arrays and camelCase names (`yawDegrees`,
`pitchDegrees`, `rollDegrees`, `distance`, `fovDegrees`, `nearPlane`). Both up axes are
supported. Set angles are absolute; yaw/roll wrap modulo 360 and pitch clamps to ±90.
Set distance must be positive and clamps to [0.001, 1e15]; target coordinates clamp
to ±1e15, FOV to [1, 179], and near plane to [0.0001, max(distance, 10)/2].
Non-finite values are rejected. Look-at rejects eye/target coordinates outside ±1e15
and separation outside [0.001, 1e15], including coincident points. At a pole it retains
the previous yaw to define roll. Near-plane normalization also applies to look-at.
Every command returns the applied camera so clients can observe normalization.

For a reproducible capture, use an instance ID from `woby ctl instances --json`:

```powershell
woby ctl --instance main camera look-at --eye 10 20 30 --target 0 0 0 --json
woby ctl --instance main camera set --roll-degrees 0 --fov-degrees 45 --json
woby ctl --instance main screenshot C:\output\camera.png --json
```

The screenshot uses the current scene, camera, and export settings. Commands execute
in order, and capture waits for GPU readback and PNG writing. Capture does not reframe
or reset the camera. The exported canvas can have a different aspect ratio from the
window. Run `uv run tests/ctl_camera_smoke.py PATH_TO_WOBY_EXE` to verify actual rendered
pixels, camera restoration, object framing, and save/load with a temporary viewer.

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
| `performance get` | `performance.get` | Last completed frame's stage/total/CPU timings and FPS, plus current drawable size and scene viewport rectangle in pixels. GPU timing is null when unavailable. |

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
| `object bounds OBJECT_ID`, `camera frame --object OBJECT_ID --include-hidden` | Expose object bounds directly or optionally include hidden geometry when framing. |
| `visibility isolate OBJECT_ID...`, `visibility restore TOKEN` | Composite visibility changes and a retained restore snapshot with stale-object handling. |
| `screenshot PATH --width W --height H --background ... --grid BOOL --origin BOOL --overwrite` | Configurable render targets and per-capture overrides. Define overwrite/default compatibility with today's unconditional overwrite and fixed size. |
| `capture views`, `capture turntable` | Sequence camera changes and screenshots, report outputs, and define whether/how to restore camera state. Video encoding is separate. |
| `load status`, `load cancel`, or generalized `jobs list/get/wait/cancel` | The UI already has load progress/cancel callbacks; CTL needs identifiable work, progress snapshots, cancellation boundaries, and terminal outcomes. Recovery via `command` alone does not provide these. |
| `run SCRIPT`, bulk setters | Client-side sequential scripts can reuse existing FIFO/retry semantics. Server-side bulk execution needs explicit ordering, partial-failure, and cancellation rules; JSON-RPC batches are currently rejected. |
| `events poll --cursor CURSOR` / event subscriptions | Event retention, cursor lifetimes, progress/change notification contracts. |
| `model reload FILE_ID`, `model watch FILE_ID` | Reload workflow and settings/group-identity preservation across geometry changes. |
| `folder create`, `object rename/reparent/reorder/duplicate` | New hierarchy operations, fresh IDs for copies, transform preservation rules, and persistence mapping. |
| `pick`, `vertex get`, `measure distance` | Reuse hover-picking foundations, but define coordinate spaces, stable geometry references, and behavior after reload. |
| `window focus/resize/minimize/restore` | New SDL runtime adapters and platform behavior. |

Orthographic projection, clipping planes, lighting/material editing,
mesh export, and headless rendering would be new viewer capabilities,
with CTL commands added alongside their implementation.


## Saved views and camera presets

Prefix these commands with `woby.exe ctl --instance ID`:

| CLI command | RPC method | Behavior |
| --- | --- | --- |
| `view list` | `view.list` | Return saved views with string `id` and `name`, plus `activeViewId` (null when none). |
| `view create [--name TEXT]` | `view.create` | Capture the current scene and camera; return the new `viewId`. |
| `view apply VIEW_ID` | `view.apply` | Restore a saved view using the same operation as the UI. |
| `view update VIEW_ID` | `view.update` | Replace the saved view with the current scene and camera. |
| `view rename VIEW_ID --name TEXT` | `view.rename` | Rename a saved view. |
| `view delete VIEW_ID` | `view.delete` | Remove a saved view. |
| `camera view PRESET` | `camera.view` | Set orientation, preserving target, distance and FOV; reset roll. |

Presets: `front`, `back`, `left`, `right`, `top`, `bottom`, `isometric`.
They respect the scene up-axis; top/bottom use exact pole orientations.
Isometric uses a -45 degree yaw and approximately 35.264 degree elevation.

RPC parameters are `viewId` (a positive decimal string), optional/required `name`
as shown above, and `preset` for `camera.view`. Obtain IDs from `view list` or
`view create`; IDs are local to the running instance and must be refreshed after
scene loading or restart. Unknown IDs fail without changing the scene. Names need
not be unique. View commands return `views`, `activeViewId`, and `dirty`; commands
acting on one view also return `viewId`. Save the scene to persist view changes.

```powershell
woby.exe ctl --instance main camera view isometric
$view = woby.exe ctl --instance main view create --name "Overview" --json | ConvertFrom-Json
woby.exe ctl --instance main camera view top
woby.exe ctl --instance main view apply $view.viewId
woby.exe ctl --instance main scene save
```

## Self-intersections

```powershell
woby ctl analysis run ANALYSIS --detector self_intersections
woby ctl analysis results ANALYSIS --json
woby ctl analysis cancel ANALYSIS --detector self_intersections
woby ctl analysis set ANALYSIS --auto-update-self-intersections true --show-self-intersections true
```

`analysis.run` requests one check of current inputs, including Update or Rerun.
It returns immediately. Poll `analysis.results` for the detector's status.
`analysis.cancel` cancels that detector only. These actions do not dirty the scene.
Results wait for ordinary requested stages and return the independent intersection
status; reading results never starts a manual check.

JSON settings: `autoUpdateSelfIntersections` (defaults false),
`showSelfIntersections`. Legacy `selfIntersections` remains an auto-update alias.
Statuses: `not_checked`, `queued`, `running`, `out_of_date`, `canceled`, `failed`,
`complete`, `partial`, or `unavailable`. Cancel suppresses auto-update until a
new request, a geometry change, or a relevant detector-settings change.

Each populated side exposes `detectors.self_intersections`: status, error,
algorithm `woby-exact-rational-intersections-v1`, topology mode, scope, nullable
exact pair `count` and `affectedFaceCount`, `knownCount`, `knownAffectedFaceCount`,
`excludedCollapsedFaces`, `unavailableSources`, and `candidateTests`.
Findings contain two source/part/generated triangle references, source provenance,
and resolved topology mode. Triangle IDs are one-based; pairs are deterministic.

JSON returns the first 100 retained pairs. `findingsTruncated` describes omitted
findings; `detectionTruncated` indicates detection stopped at the configured pair or
candidate limit (defaults: 10,000 pairs / 1,000,000 candidates). Partial/unavailable results have null exact totals.
Unchecked, queued, running, outdated, canceled, and failed states have null totals
and no current findings. `previousCount` is a previous known pair count, if one
exists; it does not describe current geometry. UI navigation covers all current
retained pairs. The eye changes visibility only.

Scene version 15 persists auto-update and display settings, including saved views.
Requests and results are not saved. Version 13 enabled flags migrate to auto-update.
Screenshots wait for requested visible checks; they do not initiate manual checks
and may include a report indicating incomplete coverage.

Checks are exact for finite double world coordinates, within each source file:
coplanar overlap and unrelated-topology contact are included; valid shared vertices
and edges are excluded. Duplicate faces are included even with matching vertex IDs.
Source transforms apply; display offsets do not. Collapsed faces are excluded.
No proximity epsilon is used.


## Complete diagnostic data, pagination, and export

`analysis results` keeps its bounded summary format. `analysis findings` (RPC
`analysis.findings`) reads one current detector and side without recomputing it:

```powershell
woby ctl --instance review analysis findings ANALYSIS_ID --side a --detector duplicate_points --offset 100 --limit 100 --json
woby ctl --instance review analysis findings ANALYSIS_ID --side a --detector duplicate_points --collection /findings/100/members --offset 100 --limit 100 --json
woby ctl --instance review analysis findings ANALYSIS_ID --side a --detector holes --collection /findings/0/vertices/120/pointReferences --json
```

`collection` defaults to `/findings`. Offsets and every index in a collection path
are **zero-based**; `analysis focus --index` remains one-based. The response includes
`items`, `total`, `nextOffset` (null at the end), detector `metadata`, and `revision`.
Pass `--revision TEXT` from the first page on subsequent requests to reject changed
results. Restart pagination if it is rejected. Pages contain 1�100 entries (default
100); nested arrays in each entry retain the summary bounds and can be paged by
extending the collection path. An offset beyond the end returns an empty page.

Accessible collections include `/sources`, `/affectedFaces`, `/boundaryRegions`,
and `/findings`. Nested paths follow the JSON field names: `members`,
`incidentFaces`, `pointReferences`, `endpoints/0/sourcePoints`, `vertices`,
`vertices/0/pointReferences`, `edgeIds`, `incidentFacesByEdge`, `faces`,
`physicalBoundaryEdgeIds`, and `cutBoundaryEdgeIds`. Select a detector that has the
requested collection. Run `analysis run ANALYSIS_ID --detector KEY` first when its
results are not current. Finding order matches the summary and focus commands.

```powershell
woby ctl --instance review analysis export ANALYSIS_ID --path D:\exports\results.json --json
woby ctl --instance review analysis export-status --json
woby ctl --instance review analysis export-cancel --json
```

RPC methods are `analysis.export` (`target`, `path`), `analysis.export-status`, and
`analysis.export-cancel` (no method-specific parameters). Export waits for fresh
results using the same rules as `analysis results`, then returns a background job
status. It does not initiate detectors with automatic updates disabled. Status
returns an `export` object with `state` (`running`, `complete`, `canceled`, or `failed`)
and `entriesWritten` counting
both findings and nested entries. Before any export it reports `idle`. The outer `state` belongs to the automation
command itself; use `export.state` to track the export job. Poll status
until terminal; cancellation is cooperative and may race with successful completion.

Progress is published in batches; the terminal `entriesWritten` count is exact.
Status includes `timings.snapshotMs` after preparation. Terminal status also reports
`bytesWritten` (bytes successfully submitted to the stream before completion or
cleanup), and `timings.writeMs`, `serializationMs`, `streamWriteMs`, `publishMs`,
and `totalMs`. `writeMs` covers serialization and writing, including stream close;
`streamWriteMs` measures the buffered stream writes and close, and
`serializationMs` is the remaining wall time. These are not CPU-time or physical
disk-durability measurements: filesystem caching and thread scheduling still
affect them. `totalMs` starts at snapshot preparation, excludes result-readiness
waiting and status polling, and is recorded before the worker releases its snapshot.

Only one export can run per viewer, and status retains the latest job. Export
captures diagnostic data once, so later edits do not change the file. Capturing the
snapshot requires memory proportional to the retained diagnostics; writing then
streams entries without constructing a complete JSON document in memory. Render
buffers and distance sample arrays are not copied for export.

The final file is ordinary JSON with the results summary and complete retained
arrays on both sides. `allRetainedResults: true` means there was no listing cap;
`detectionComplete` remains false if any populated side has an incomplete detector.
Per-detector statuses, exact/known counts, and detection truncation are preserved.
Unchecked or canceled detectors do not become complete through export.

The destination parent must already exist. CLI paths may be relative; RPC paths
must be absolute. Export never overwrites an existing destination. It writes in a
reserved sibling directory and publishes only on success using a same-filesystem
no-overwrite rename on Windows, or a hard link on other platforms (which require
hard-link support). Cancellation or
failure removes temporary output. Viewer shutdown cancels and waits for the job.
The large file is outside the 8 MiB automation response history; only the small job
status is retained there.

### Self-intersection computation budgets

`analysis set` accepts `--intersection-pair-limit N` and
`--intersection-candidate-limit N` (RPC `intersectionPairLimit` and
`intersectionCandidateLimit`). Defaults remain 10,000 retained pairs and 1,000,000
candidate tests **per side**, shared across its source files. Each value accepts
0�2,147,483,647; **0 disables that budget**. These settings persist in `.woby` files
and saved views, and are also editable through the self-intersection settings icon.
Changing them invalidates current intersection results; automatic checks rerun,
while manual checks require `analysis run ... --detector self_intersections`.

Results report the actual `pairLimit` and `candidateLimit` used, plus
`truncationReason` (`pair_limit`, `candidate_limit`, or null). When a budget stops
detection, `detectionTruncated` is true, exact totals are null, and known counts
refer only to retained results. Exporting or paging does not resume detection.
Unlimited dense intersections can take quadratic time and storage; cancel with
`analysis cancel ANALYSIS_ID --detector self_intersections`.

## Fin candidates

`analysis run ANALYSIS --detector fins` and `analysis cancel ANALYSIS --detector fins`
control the fin candidate lifecycle. `analysis set` accepts `--fins BOOL` for
automatic updates, `--show-fins BOOL` for visibility, and
`--fin-max-area-ratio NUMBER` for the inclusive area filter (default 1, minimum 0).
Non-finite values are rejected by the control protocol.

Each populated side of `analysis results --json` includes `detectors.fins`, with
algorithm `woby-fin-candidates-v1`, `heuristic: true`, status, nullable exact count,
known count, maximum area ratio, and at most 100 patch findings. Each patch
reports source and topology mode, patch ID, split-component count, physical
boundary classification, area, denominator area, ratio, source face references,
and separate physical/cut boundary edge IDs. Face and edge lists are bounded to
100 entries each, with full counts and truncation flags. IDs are 1-based.
Unavailable sources produce partial/unavailable status and null exact counts.
Stale, canceled, failed, and unchecked results expose no current findings.

Patches are connected through manifold edges only. At least two split patches
per source are required. Candidate physical boundaries must be open, branched,
or multiple loops. Cut edges are not counted as physical boundaries. The area
denominator is the largest physical-boundary-bearing split patch per source,
before boundary-shape filtering, including ordinary simple-loop disks. Closed
patches are excluded. This is a Woby heuristic; intentional multi-opening sheets
can qualify, and GGM/GMM parity is not claimed.

If a physical-boundary patch area cannot be represented as a positive finite
double, fin results for that source are unavailable (`unavailableAreaSources`);
other topology detectors remain usable.


### Coordinate origins

`scene.info.coordinateOrigin` is the double-precision original-coordinate offset
of the working scene. It is fixed after the first file is added and saved in the
scene. Rendering, camera targets, and camera placement commands use working
coordinates. Add this origin to a working position to recover its original world
coordinate. Object local bounds, annotation vertex readouts, and detector finding
positions already include the appropriate offset. Distances and sizes need no
offset. Imported positions and detector calculations retain double precision;
GPU positions remain floats relative to their mesh origin.
