# Difference mesh

In a mesh comparison with inputs A and B, select **Create difference mesh...**.
Choose the method, then select **Create difference mesh**. The **View** list also
contains **Difference mesh**. Opening these controls does not start a build.
Work runs in the background.
You can cancel a build. Cancellation prevents publication of its result; an active
Boolean operation can take time to stop.

For solids, the result has two parts:

- **A only**, in blue: volume in A that is absent from B.
- **B only**, in orange: volume in B that is absent from A.

Total changed volume is the sum of these parts. Net change is B-only volume minus
A-only volume. Net change can be zero when the shapes differ. Volumes use model
units cubed. Acceptance tolerance and the heatmap color range do not change the
geometry or these volumes.

## Direct mesh

**Direct mesh** is the default. For open surfaces, automatic closure requires one simple boundary loop on each
input. The tool matches the loops and adds walls. It supports different boundary
vertex counts. The result is labelled **Constructed gap volume**. The generated
walls have a separate visibility control and a gold color. These walls are an
assumption; they are not measured source geometry.

Before validation, the tool removes input triangles with exactly zero area from
the calculation. The source meshes stay unchanged. The result message gives the
removed count for A and B when either count is nonzero. Small triangles with
nonzero area remain. If no triangles remain in an input, creation stops with an
error that names the input and gives the removed count.

The tool stops and shows a problem for holes, branching boundaries, mixed open
and closed inputs, ambiguous loop direction, inconsistent winding,
self-intersections, crossing surfaces, and invalid closures. It does not
repair the sources or provide boundary editing. Exact coincident positions are
joined for topology checks. Nearby positions are not joined by a distance tolerance.
Overlapping solid components within one input must be resolved before creation.
Validation has a work limit; very complex inputs can require smaller input groups.

## Approximate volume

Select **Approximate volume** for models that represent solids but contain holes,
non-manifold edges, or intersecting faces. Enter **Cell size** in model units.
This method estimates the interior from generalized winding numbers. It does
not require valid input connectivity. Triangle orientation must be reasonably
consistent. A global winding reversal is accepted. Oppositely oriented internal
shells can represent cavities; arbitrary face reversals can change the result.

The method reconstructs both inputs on the same body-centered cubic sampling
grid, then subtracts the reconstructed solids in both directions. Cell size sets
the grid spacing; the second grid is offset by half a cell on each axis. A tree
estimates distant triangle contributions with an error bound. Ambiguous
classifications use exact solid-angle sums. Surface extraction uses Manifold's
marching tetrahedra implementation. Reported volumes are measured from the
reconstructed difference meshes, not from the input triangle volume sums.

Smaller cells retain more detail and use more memory and time. Thin features can
be lost. Missing surfaces are inferred; large missing areas do not have a unique
answer. The result is labelled **Approximate**. If no solid is found, use smaller
cells and check orientation. Two open sheets need a separate closure rule; this
method does not construct the gap between them. There is no grid gap-closing
setting in this version.

Parametric inputs use their existing tessellation. A smaller cell size does not
recover curvature lost during tessellation. Both triangle and parametric input
types use the same controls.

The grid has an eight-million-sample allocation limit, including internal index
padding. A request beyond the limit reports a problem and asks for larger cells
or smaller input groups. The method does not silently reduce the requested detail.

**Check at half the cell size** runs a separate background calculation. It reports
A-only, B-only, and total volumes at the smaller cell size, with the change in
total volume. It does not replace the displayed result. This is a resolution
sensitivity check, not an error bound or a test of the inferred missing shape.
You can cancel the check. Changes to inputs or construction settings invalidate it.

## Display and saved settings

Use the eye controls to show each part, and the opacity slider to see through it.
**Frame result** frames the comparison. Comparison placement changes the display
position only. Source transforms change the computed geometry.

Source geometry, transforms, membership, method, or active cell-size changes mark the result out of date.
Use **Rebuild difference mesh** to update it. An outdated result is not displayed
or exported. Scene files save the settings and source links; loading the scene
rebuilds a previously created result. Method and cell size are saved even before
creation. Older scenes use Direct mesh. Generated triangles and resolution-check
results are not embedded in the scene file.

The inspector has no export controls. The existing CLI export writes every result part, including hidden parts, in source world
coordinates. It excludes the comparison display offset. It requires a new file.
OBJ groups identify A-only, B-only, gap surfaces, and closure walls. Colors and
opacity are display settings and are not written to OBJ.

## CLI

Use a mesh comparison ID as `ANALYSIS_ID`. The normal CLI target rules apply.

```text
woby ctl --instance demo analysis difference-create ANALYSIS_ID
woby ctl --instance demo analysis difference-create ANALYSIS_ID --method approximate --cell-size 0.1 --wait --json
woby ctl --instance demo analysis difference-set ANALYSIS_ID --method approximate --cell-size 0.05
woby ctl --instance demo analysis difference-get ANALYSIS_ID
woby ctl --instance demo analysis difference-set ANALYSIS_ID --show-a-only true --show-b-only false --opacity 0.6
woby ctl --instance demo analysis difference-set ANALYSIS_ID --show-walls false --visible true
woby ctl --instance demo analysis difference-rebuild ANALYSIS_ID
woby ctl --instance demo analysis difference-rebuild ANALYSIS_ID --method approximate --cell-size 0.05 --wait --json
woby ctl --instance demo analysis difference-check-resolution ANALYSIS_ID --wait --json
woby ctl --instance demo analysis difference-cancel-check ANALYSIS_ID
woby ctl --instance demo analysis difference-cancel ANALYSIS_ID
woby ctl --instance demo camera frame --object ANALYSIS_ID
woby ctl --instance demo analysis difference-export ANALYSIS_ID --path C:/output/difference.obj
woby ctl --instance demo analysis difference-delete ANALYSIS_ID
```

Create, rebuild, check, and export complete after their background work finishes.
The CLI always waits; `--wait` is accepted for compatibility. Use `--timeout SECONDS`
for longer jobs. You can run `difference-get` or a cancel command from another
terminal while a difference command waits. `difference-cancel-check` keeps the
base result. `difference-cancel` cancels the build and any resolution check.
If the CLI times out, work can still finish. Use `--request-key KEY` on the initial
command and `woby ctl --instance demo command --request-key KEY --json` to get its
completion state without starting another job.

`difference-get` returns a `difference` object with the state, problem
text, display settings, volumes, triangle counts, and export status. Volume fields
are null when the result is unavailable or out of date. A constructed gap has a
total volume, but no A-only, B-only, or net volume values.
For a current result, `removedCollapsedTriangles.a` and
`removedCollapsedTriangles.b` give the counts of exact zero-area input triangles
removed from the calculation.
`settings.method` and `settings.cellSize` identify the requested construction
settings. A current approximate result has `kind = "approximate_volume"` and its
computed `cellSize`. `resolutionCheck` reports the check state and includes volumes
only for a completed check of the current result. `difference-check-resolution`
requires a current approximate result. Its response and `difference-get` include
`baseCellSize`, the check `cellSize`, `volumeA`, `volumeB`, `netVolume`, and `totalVolume`.
`totalVolumeChange` is the check total minus the base total. `relativeChangePercent`
is 100 times that change divided by the base total, or null when the base total is
zero. These values measure sensitivity to resolution, not accuracy.

Configuration is allowed before
creation. Direct and approximate methods share the existing visibility controls.
The CLI uses the same input path for triangle and parametric models. Save the scene
with `scene save` or `scene save-as` to keep method, cell size, and display settings.
The RPC methods have the same names with an `analysis.` prefix, for example
`analysis.difference-check-resolution`, with a `target` analysis ID.
