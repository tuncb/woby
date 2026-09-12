# Duplicate inspection

Open `inspect.woby`, select **Inspect duplicates**, and expand the properties pane.
Under **Diagnostics**, use the arrows on the **Duplicate points** or
**Duplicate triangles** row to visit groups, just like the edge rows. Select a
detector row to see its detailed findings list. **Full result** restores the full analysis view.

Expected results:

- Duplicate points: **2 extra records in 2 groups** (point 5 repeats point 1;
  unused point 6 repeats point 3). IDs shown in the UI are 1-based.
- Duplicate triangles: **2 extra records in 1 group** (triangles 1, 2, 3).
  Triangle 2 has the same cyclic order; triangle 3 is reversed.
- Geometric duplicate triangles: **3**. Triangle 4 overlaps triangle 1 but
  references point 5, so it is excluded from the source-ID triangle detector.

Point markers are cyan; duplicate triangles have orange fill and outlines.
The selected group is yellow. Overlapping records share one highlight, while
the details list retains every source ID. Run and Show are independent controls.
