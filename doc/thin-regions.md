# Thin regions

Create a mesh-check analysis and add the model surface to its sources. Run
**Thin regions**. Use its settings button to set the limits. Use the eye button
to show or hide the result. Select a region to frame it and show a measurement
line between its two surfaces.

The detector samples four points on each triangle. It casts seven rays at each
point: one inward normal ray and six rays in a cone with a 25-degree half-angle.
A sample needs at least three opposing-surface hits within 1.5 times its shortest
hit. A face needs at least two supported samples. The typical thickness is the
upper median of those sample lengths. The minimum is the shortest supported
sample. These values estimate thickness; they do not guarantee the global
minimum or FEM accuracy. Small features between sample points can be missed.

The detector joins exact coordinate seams for this check. It requires closed,
manifold, consistently wound surfaces. It checks for self-intersections before
measuring thickness. Nested shells define cavities by their nesting parity.
Global winding reversal is supported. Sources with invalid boundaries are
unavailable. Faces with too few supported rays are unknown.

## Limits and display

- **Absolute thickness:** Compare the typical thickness with a limit in model units.
- **Relative thickness:** Divide typical thickness by the area-weighted 95th
  percentile in the same analysis batch. This is a batch reference, not a local
  neighborhood reference. Use the analysis processing mode to choose the batch scope.
- A face passes if either enabled limit passes. Disable both limits to show only
  the thickness map without region findings.
- **Minimum region surface area:** Remove smaller findings. Area includes both
  sides of a wall. Adjacent selected faces and selected opposite faces form a region.
- **Thickness map:** Red is thin. Blue is reference thickness or greater. Gray
  is unknown. Each batch has its own reference. Region edges remain visible.
- **Elements through thickness:** Divide the sampled minimum thickness by this
  number for a preliminary element-size limit. The default is three. Choose a
  value for the intended element type and analysis. Element growth limits and
  the extent of surrounding refinement are not calculated.

Confidence is the supported ray fraction, not a probability or an error bound.
Changes to limits reuse cached measurements. Geometry changes require new
measurements. Automatic updates are off by default. Computation runs in a
cancelable background job.

## Control API

```text
woby ctl analysis run ANALYSIS_ID --detector thin_regions
woby ctl analysis set ANALYSIS_ID --thickness-absolute-enabled true --maximum-thickness 1
woby ctl analysis set ANALYSIS_ID --thickness-map true --thickness-elements 3
woby ctl analysis findings ANALYSIS_ID --detector thin_regions --collection /findings
```

Use `/findings/0/faces` for the source triangle references in the first region.
Full analysis export includes all retained region and face records. Exported
measurement endpoints use the original world coordinates. Scenes and review
views save all detector controls. Unknown results remain explicit in exports.
