# Difference mesh

In a mesh comparison with inputs A and B, select **Create difference mesh**.
The **View** list also contains **Difference mesh**. Work runs in the background.
You can cancel a build. Cancellation prevents publication of its result; an active
Boolean operation can take time to stop.

For closed solids, the result has two parts:

- **A only**, in blue: volume in A that is absent from B.
- **B only**, in orange: volume in B that is absent from A.

Total changed volume is the sum of these parts. Net change is B-only volume minus
A-only volume. Net change can be zero when the shapes differ. Volumes use model
units cubed. Acceptance tolerance and the heatmap color range do not change the
geometry or these volumes.

For open surfaces, automatic closure requires one simple boundary loop on each
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

Use the eye controls to show each part, and the opacity slider to see through it.
**Frame result** frames the comparison. Comparison placement changes the display
position only. Source transforms change the computed geometry.

Source geometry, transforms, or membership changes mark the result out of date.
Use **Rebuild difference mesh** to update it. An outdated result is not displayed
or exported. Scene files save the settings and source links; loading the scene
rebuilds the result. Generated triangles are not embedded in the scene file.

**Export OBJ** writes every result part, including hidden parts, in source world
coordinates. It excludes the comparison display offset. It requires a new file.
OBJ groups identify A-only, B-only, gap surfaces, and closure walls. Colors and
opacity are display settings and are not written to OBJ.

## CLI

Use a mesh comparison ID as `ANALYSIS_ID`. The normal CLI target rules apply.

```text
woby ctl --instance demo analysis difference-create ANALYSIS_ID
woby ctl --instance demo analysis difference-get ANALYSIS_ID
woby ctl --instance demo analysis difference-set ANALYSIS_ID --show-a-only true --show-b-only false --opacity 0.6
woby ctl --instance demo analysis difference-set ANALYSIS_ID --show-walls false --visible true
woby ctl --instance demo analysis difference-rebuild ANALYSIS_ID
woby ctl --instance demo analysis difference-cancel ANALYSIS_ID
woby ctl --instance demo analysis difference-export ANALYSIS_ID --path C:/output/difference.obj
woby ctl --instance demo analysis difference-delete ANALYSIS_ID
```

Create, rebuild, and export complete after their background work finishes. Use the
standard CLI wait/timeout options. `difference-get` returns a `difference` object with the state, problem
text, display settings, volumes, triangle counts, and export status. Volume fields
are null when the result is unavailable or out of date. A constructed gap has a
total volume, but no A-only, B-only, or net volume values.
For a current result, `removedCollapsedTriangles.a` and
`removedCollapsedTriangles.b` give the counts of exact zero-area input triangles
removed from the calculation.
