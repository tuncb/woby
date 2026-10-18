# Difference mesh

In a mesh comparison with inputs A and B, select **Create difference mesh...**,
then **Create difference mesh**. The **View** list also contains **Difference mesh**.
Opening these controls does not start a build. The calculation runs in the
background. Cancellation prevents publication of its result; an active Boolean
operation can take time to stop.

The result has two parts:

- **A only**, in blue: volume in A that is absent from B.
- **B only**, in orange: volume in B that is absent from A.

Total changed volume is the sum of these parts. Net change is B-only volume minus
A-only volume. Net change can be zero when the shapes differ. Volumes use model
units cubed. Acceptance tolerance and the heatmap color range do not change the
geometry or these volumes.

## Calculation and limits

All differences use the fast approximate mesh calculation. It uses the input
triangles and the existing Manifold library with floating-point geometry. It
works near a common local origin to reduce numerical cancellation. Thin changes
can remain visible without a sampling grid. There is no method selector, cell
size, resolution check, or automatic closure of open surfaces.

Inputs must have closed, consistently oriented topology. Surface intersections
are accepted without an exact intersection check. The calculation checks library
status, finite coordinates, closed output topology, and basic volume consistency.
These checks do not establish an accuracy bound. Volumes are estimates and assume
that input surfaces represent valid solids. Overlaps or other geometric defects
can affect the result. The result is labelled **Approximate**.

Source meshes stay unchanged. The calculation does not repair meshes, close holes,
increase the Boolean tolerance, or simplify the result. It joins exact coincident
input positions and omits triangles with exactly zero area from the calculation.
Small triangles with nonzero area remain. The result reports the omitted count
for A and B. If no triangles remain, creation stops with an input-specific error.
Open or non-manifold topology also produces an input-specific error.

Parametric inputs use their current tessellation. The calculation cannot recover
curvature lost during tessellation.

To include changes between geological units, create one comparison for each
matched unit. A comparison of the combined outer volume can hide an internal
boundary change. The sum of per-unit changes is not a unique changed spatial
volume: a region that moves from one unit to another occurs in both units.

## Display and saved settings

Use the eye controls to show each part, and the opacity slider to see through it.
**Frame result** frames the comparison. Comparison translation changes the display
position only. The difference uses the comparison's input alignment and placement.
With placement unlocked, source transforms change the computed geometry. With
placement locked, source movement does not change the saved input placement.

Changes to source geometry, input placement, or membership mark the result out
of date. Use **Rebuild difference mesh** to update it. An outdated result is not
displayed or exported. Display settings do not invalidate a result.

Scene files save creation state, display settings, and source links. Loading a
scene rebuilds a previously created result. Generated triangles are not embedded
in the scene file. Method, cell-size, and closure-wall settings are no longer saved
or read. There is no migration for the removed calculation methods.

The inspector has no export controls. CLI export writes both result parts,
including hidden parts, in source world coordinates. It excludes the comparison
display offset and requires a new file. OBJ groups identify `A_only` and `B_only`.
Colors and opacity are display settings and are not written to OBJ.

## CLI

Use a mesh comparison ID as `ANALYSIS_ID`. The normal CLI target rules apply.

```text
woby ctl --instance demo analysis difference-create ANALYSIS_ID --json
woby ctl --instance demo analysis difference-get ANALYSIS_ID --json
woby ctl --instance demo analysis difference-set ANALYSIS_ID --show-a-only true --show-b-only false --opacity 0.6
woby ctl --instance demo analysis difference-set ANALYSIS_ID --visible true
woby ctl --instance demo analysis difference-rebuild ANALYSIS_ID --json
woby ctl --instance demo analysis difference-cancel ANALYSIS_ID
woby ctl --instance demo camera frame --object ANALYSIS_ID
woby ctl --instance demo analysis difference-export ANALYSIS_ID --path C:/output/difference.obj
woby ctl --instance demo analysis difference-delete ANALYSIS_ID
```

Create, rebuild, and export complete after their background work finishes. Use
`--timeout SECONDS` for longer jobs. You can get status or cancel from another
terminal while a difference command waits. If the CLI times out, work can still
finish. Use `--request-key KEY` on the initial command and
`woby ctl --instance demo command --request-key KEY --json` to get its completion
state without starting another job.

`difference-get` returns a `difference` object with the state, problem text,
display settings, volumes, two triangle counts, and export status. Volume fields
are null when the result is unavailable or out of date. A current result has
`kind = "approximate_mesh"`. `removedCollapsedTriangles.a` and
`removedCollapsedTriangles.b` give the omitted exact zero-area triangle counts.

`difference-set` accepts display settings before or after creation. Save the scene
with `scene save` or `scene save-as` to keep them. Removed method, cell-size, and
closure-wall options and resolution-check commands are rejected. RPC methods have
the same names with an `analysis.` prefix, for example `analysis.difference-create`,
with a `target` analysis ID.
