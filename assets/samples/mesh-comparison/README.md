# Mesh comparison sample

Open `compare.woby` in woby to start directly in comparison mode. Both files use
millimeters. The viewer itself reports **model units** because OBJ/STL files do
not provide a dependable unit declaration.

The original is a small triangulated sheet with a hole, a raised bump, and a
detached triangle. The repaired sheet fills the hole, reduces the bump, and
removes the detached triangle. The outer edge of the sheet intentionally stays
open; these sample meshes are surfaces, not closed solids.

The original surface belongs to **A** and the repaired surface belongs to **B**.
Right-click any scene-tree file, folder, or mesh part to add/remove its current
mesh parts from either group. Ctrl-click selects multiple objects. Adding a
parent includes each part once; removing a child removes just that part. New
children are not automatically added. A part may belong to both sides.

The **Comparison** panel opens on the right when you add objects or start comparing.
Use the upper-right **A/B** button to hide/show it. Its trees show each side using
the scene hierarchy; right-click a branch or part to remove it from that side.

Try these controls in **Comparison**:

1. **Surface distance** shows the repaired surface's unsigned distance from the
   original. The filled patch and flattened bump should stand out.
2. Enable **Measure on A** to see the removed detached triangle.
3. Switch between **Group A** and **Group B** without moving the camera.
4. **Overlay** draws the original edges in blue over the repaired surface.
5. Enable **Boundary edges** and inspect the A/B edge counts. The
   original has 47 boundary edges; the repaired surface has 36. **First** focuses
   the first edge of that category on the displayed mesh.
6. Change the tolerance and color maximum, then save/reopen the `.woby` file.

The example contains 153 original triangles and 160 repaired triangles.

Prototype limits:

- Up to 50,000 triangles in total per comparison group. Distance computation runs in the
  background; changing transforms invalidates the result and recomputes it.
- Comparison isolates the combined member surfaces and uses their scene hierarchy
  transforms. Membership changes also recompute the result. Ordinary group visibility and appearance do not limit comparison.
- Distances use four triangle-interior samples per source face and closest points
  on the other mesh's triangles, accelerated with a bounding-volume tree. This
  is an approximation, not an exact Hausdorff bound. Small features can fall
  between samples. Mean, P95, and area percentages use sample surface area.
- Distances are unsigned and no automatic alignment or Boolean union is performed.
  Overlapping and internal surfaces remain part of the comparison.
- Edge diagnostics merge exactly equal positions to handle split OBJ/STL
  vertices, including coincident edges across different parts; they do not merge
  nearby unequal positions. Degenerate faces are
  counted separately and excluded from edge incidence checks.
- Counts and edge overlays cover boundaries, non-manifold edges, inconsistent
  winding, duplicate triangles, and degenerate triangles. Self-intersections
  and operation-history-based added/removed classifications are not implemented.
- Screenshot export includes the comparison scene but not the UI legend.
