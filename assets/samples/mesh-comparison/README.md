# Mesh comparison sample

Open `compare.woby` in woby to start directly in comparison mode. Both files use
millimeters. The viewer itself reports **model units** because OBJ/STL files do
not provide a dependable unit declaration.

The original is a small triangulated sheet with a hole, a raised bump, and a
detached triangle. The repaired sheet fills the hole, reduces the bump, and
removes the detached triangle. The outer edge of the sheet intentionally stays
open; these sample meshes are surfaces, not closed solids.

Try these controls in **Compare meshes**:

1. **Surface distance** shows the repaired surface's unsigned distance from the
   original. The filled patch and flattened bump should stand out.
2. Enable **Measure on original** to see the removed detached triangle.
3. Switch between **Original** and **Repaired** without moving the camera.
4. **Overlay** draws the original edges in blue over the repaired surface.
5. Enable **Boundary edges** and inspect the before/after edge counts. The
   original has 47 boundary edges; the repaired surface has 36. **First** focuses
   the first edge of that category on the displayed mesh.
6. Change the tolerance and color maximum, then save/reopen the `.woby` file.

The example contains 153 original triangles and 160 repaired triangles.

Prototype limits:

- Up to 50,000 triangles per selected file. Distance computation runs in the
  background; changing transforms invalidates the result and recomputes it.
- Comparison isolates the selected whole files and uses their scene hierarchy
  transforms. Ordinary group visibility and appearance do not limit comparison.
- Distances use four triangle-interior samples per source face and closest points
  on the other mesh's triangles, accelerated with a bounding-volume tree. This
  is an approximation, not an exact Hausdorff bound. Small features can fall
  between samples. Mean, P95, and area percentages use sample surface area.
- Distances are unsigned and no automatic alignment is performed.
- Edge diagnostics merge exactly equal positions to handle split OBJ/STL
  vertices; they do not merge nearby unequal positions. Degenerate faces are
  counted separately and excluded from edge incidence checks.
- Counts and edge overlays cover boundaries, non-manifold edges, inconsistent
  winding, duplicate triangles, and degenerate triangles. Self-intersections
  and operation-history-based added/removed classifications are not implemented.
- Screenshot export includes the comparison scene but not the UI legend.
