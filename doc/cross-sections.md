# Cross sections

Choose **Tools > Cross section**, then drag a line across the model. Use an angled
view or Top view. The line is drawn on a horizontal workplane at the bottom of the
scene. It defines a vertical cutting plane, using the scene's current Y-up or
Z-up axis. Press Escape to cancel.

Hold **Shift** while drawing or dragging an endpoint to snap the direction to
45-degree increments on the workplane. Release Shift to return to free drawing.
The preview updates when Shift changes, even if the mouse does not move.

One annotation contains the cuts of all triangle meshes visible when you draw
the line. The plane extends beyond the line endpoints. It does not change or
remove mesh geometry. Separate contours stay separate.

Select the annotation to show its Properties panel and handles:

- Drag an endpoint to rotate the plane. Drag the line to move the plane.
- Drag the square **Lift** handle to raise or lower all contours together.
- Use **Position**, **Start**, and **End** for numeric plane edits.
- Use the eye beside a source to hide its contour. Hiding the source model does
  not remove it from the section. Draw another section to capture a different set.
- Enable **Face-on 2D** to inspect all contours in one plot with equal axis scales.
- Enable **Show 2D differences** and choose two sources. Other contours remain
  visible. The white segment marks the largest sampled gap.

The gap is the largest nearest-curve distance among up to 512 sampled endpoints
from each of the two contours. Both directions are sampled. It is an estimate,
not an exact maximum. It measures only this cutting plane, so it can differ from
the 3D distance heatmap. Display lift does not change the measurement. All values
use model units. **No cut** and **Missing source** do not mean zero distance.

Cuts update in the background after a plane or source geometry edit. **Updating**
replaces old results until the new cut is ready. PNG export waits for the cuts.
Names, comments, source visibility, plane settings, display settings, and the
comparison pair are saved in `.woby` files. Annotation duplication, deletion,
visibility, and Undo/Redo also apply to cross sections.

## Command line

Create one section from all visible triangle meshes:

```text
woby ctl --instance demo annotation create scene --shape section --plane-start -5 30 0 --plane-end 105 30 0 --lift 22 --name "Section S" --show-differences true
```

Use a folder, file, or group ID instead of `scene` to capture all triangle parts
in that object, including hidden parts. The captured source list stays fixed.
Endpoints use world coordinates. Lift uses model units. `--section-up-axis y|z`
defaults to the scene up axis. The end height is set to the start height.

Use the returned annotation ID to read or change the section:

```text
woby ctl --instance demo annotation get ANNOTATION_ID
woby ctl --instance demo annotation set ANNOTATION_ID --plane-end 105 45 0 --lift 30 --face-on true
woby ctl --instance demo annotation set ANNOTATION_ID --source-index 2 --source-visible false
woby ctl --instance demo annotation set ANNOTATION_ID --reference-index 0 --compared-index 1 --show-differences true
woby ctl --instance demo annotation set ANNOTATION_ID --show-plane false --show-connections false
```

Indices start at zero and follow `sourceIds` in `annotation get`. The source
visibility flags control contours, not the source models. Use `annotation set`
for plane edits. Surface `move` and `reshape` commands do not apply to sections.
Unlock the section before changing its plane.

Common annotation controls include `--name`, `--comments`, `--visible`,
`--locked`, `--opacity`, and `--width`. Sections also support `annotation list`,
`annotation delete`, scene save/load, and Undo/Redo. `woby ctl --help` lists the
section options.
