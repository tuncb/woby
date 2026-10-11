# Remeshing distortion example

Open `compare.woby` in Woby. It uses two OBJ files in this folder.
Keep the files together. All scene paths are relative.

The meshes represent the same curved plate, 60 by 40 mm. The second mesh has
fewer triangles, a different triangle layout, and deliberately added distortion.
This is synthetic test data. It is not output from a particular meshing tool.

| Mesh | Vertices | Triangles | Purpose |
| --- | ---: | ---: | --- |
| `original.obj` | 5,852 | 11,700 | Dense reference surface (A) |
| `remeshed-distorted.obj` | 752 | 1,500 | Coarse distorted surface (B) |

Both meshes are closed and have consistent outward face orientation. The bottom
surface is at Z = 0. The top surface is about 4 to 5 mm above it. Coordinates
and distance settings use millimeters; OBJ does not store a standard unit field.

## Scenes

- `compare.woby`: four views. With the saved camera, the original is at the top
  left and the distorted mesh is at the top right. The bottom row shows the same
  B-to-A distances with color maxima of 8 mm and 0.65 mm. Both use a tolerance of
  0.05 mm. Only the display range changes. Display translations do not change
  the measured geometry.
- `inspect.woby`: one heatmap. Select its analysis in the scene tree to change
  the range, direction, display mode, and triangle edges. Start with 8 mm, then
  set Color maximum to 0.65 mm. Select Fit color range to fit the sampled maximum.
- `triangle-edges.woby`: two overlays with Triangle edges off and on. In the
  current implementation, the blue reference wireframe remains visible in both.
  The switch controls a separate dark edge layer on the measured mesh.

Source models are hidden to prevent them from obscuring the analysis results.
Each analysis still uses both source models in their original shared coordinates.

## Added distortion

The coarse mesh has small waves across its top, a broad raised region near
(19, 15), a shallow dent near (39, 25), and a narrow spike near (46, 10).
The spike vertex is about 9 mm above the reference. Woby samples triangle
interiors, so its sampled maximum is lower than the spike's vertex displacement.
Distances are unsigned: the dent and raised region use the same color scale.

The reference top uses this height function, with X and Y in millimeters:

`Z = 4 + 1.1 sin(pi X/60) sin(pi Y/40) + 0.5 sin(2 pi X/60) sin(pi Y/40)`

The original top has a 90 by 60 cell grid. The distorted top has a 30 by 20 cell
grid, small changes to interior X/Y positions, and alternating triangle diagonals.

## Checked results

The files were loaded and rendered in the existing Woby 0.28.0 Release build.
The current four-samples-per-triangle calculation gives these approximate values:

| Measurement | B to A | A to B |
| --- | ---: | ---: |
| Sample maximum (mm) | 6.12179 | 1.79589 |
| Area-weighted mean (mm) | 0.06288 | 0.02390 |
| Area-weighted P95 (mm) | 0.17960 | 0.14636 |
| Area above 0.05 mm (%) | 12.0118 | 11.1459 |

These area statistics include the unchanged bottom and side surfaces. They are
not top-surface-only statistics. The sampled maximum is not an exact maximum.

Both input meshes report zero boundary edges, non-manifold edges, winding
conflicts, duplicate triangles, and degenerate triangles. Self-intersection
detection was not run.

The screenshots use Woby's renderer:

- `preview-wide.png`: color maximum 8 mm; most errors look almost the same.
- `preview-detail.png`: color maximum 0.65 mm; the broad error regions are easier
  to distinguish. Larger distances use the top color but retain their values.
- `preview-edges-off.png`: blue wireframe remains visible with Triangle edges off.

The four-view scene works in the viewport. The current default PNG export can
reject it because four analysis reports do not fit in the annotation panel.
Export one visible analysis at a time, as in the supplied screenshots, or increase
the export image height in the desktop export dialog.
