# Surface mesh quality samples

Open any `.woby` file below in Woby. All OBJ files are included and use relative
paths, so the whole folder can be copied elsewhere. Coordinates are in millimeters.
No generator, plugin, or download is needed to open them.

The projects open in **Surface mesh quality** mode with a saved camera. Paired
examples display A on the left and B on the right as two named analyses that
share the same inputs. Select either analysis in the scene tree to inspect
the A/B statistics, histogram, metric selector, and size limits in Properties.
Source meshes are hidden to avoid covering the heatmaps, but remain analysis inputs.
Changing one analysis's metric does not change the other; select the same metric
on both when comparing their colors. Each analysis computes its range from both inputs.

| Project | Starts with | What to verify |
| --- | --- | --- |
| [01-coarse-vs-fine.woby](01-coarse-vs-fine.woby) | Longest edge | Same 6 x 6 sheet, 18 versus 72 triangles. Size changes while shape stays the same. |
| [02-equal-area-different-shapes.woby](02-equal-area-different-shapes.woby) | Shape quality | Four equilateral triangles versus four progressively thinner triangles with the same area. |
| [03-gradual-vs-abrupt.woby](03-gradual-vs-abrupt.woby) | Local size jump | Same rectangular domain with gradually increasing versus abruptly changing strip heights. |
| [04-size-limits-and-area.woby](04-size-limits-and-area.woby) | Longest edge | One of two triangles exceeds the limit: 50% of triangles but 80% of surface area. |
| [05-topology-and-unavailable.woby](05-topology-and-unavailable.woby) | Local size jump | Boundaries, a non-manifold fan, winding, duplicate faces, degenerate faces, and unavailable neighbor measurements. |

## 1. Coarse versus fine

- A: 18 triangles, longest edge `2*sqrt(2) = 2.828427` mm.
- B: 72 triangles, longest edge `sqrt(2) = 1.414214` mm.
- Both: every shape value is `sqrt(3)/2 = 0.866025`; local size jump is 1.
- Maximum size is 1.5 mm: A has 100% of faces and area above the limit; B has 0%.
- Switch both analyses to **Equivalent size**. A is `2.149140` mm and B is
  `1.074570` mm. The factor of two remains, while both shape histograms coincide.

Size colors use the common A/B range; they are not an automatic FEM pass/fail score.

## 2. Equal area, different shapes

Each triangle has area `sqrt(3)` square millimeters and equivalent size **2 mm**.
A contains four equilateral triangles. B's base lengths, in increasing world-Y
row order, are 2, 4, 8, and 16 mm; their heights decrease to preserve area.

| B base length | Shape quality |
| --- | --- |
| 2 mm | 1 |
| 4 mm | `8/17 = 0.470588` |
| 8 mm | `32/257 = 0.124514` |
| 16 mm | `128/4097 = 0.031242` |

Switch to **Equivalent size**: every triangle on both sides should have the same
value and color, despite the very different shapes. Switch to **Longest edge**
to expose the long spans. These triangles are deliberately disconnected, so
**Local size jump** is unavailable (gray, measured-face count 0, statistics N/A).

## 3. Gradual versus abrupt growth

Both meshes cover a 4 x 7.75 mm rectangle and use 1 mm wide columns.

- A has 40 triangles and strip heights 0.25, 0.5, 1, 2, 4 mm.
  Its maximum local size jump is `sqrt(2) = 1.414214`.
- B has 16 triangles and strip heights 0.25 and 7.5 mm.
  Its maximum local size jump is `sqrt(30) = 5.477226` at the strip interface.
- Both include faces with a jump of 1. Only faces incident to the size-changing
  interface inherit that interface's ratio; their partner triangles can remain 1.

The measure compares equivalent sizes, so it is the **square root of the area
ratio**, not the area ratio or the ratio of strip heights. Shape can change along
with grading; switch to **Shape quality** to inspect that separately.

## 4. Size limits: counts versus area

This single-input project contains one 3-4-5 triangle and one 6-8-10 triangle.
Their areas are 6 and 24 square millimeters, and their longest edges are 5 and 10 mm.

Both enabled limits start at **5 mm**. Endpoints are inclusive:

- Below minimum: 0. Above maximum: 1.
- Outside limits: **50% of valid faces, 80% of their total area**.
- Longest-edge min/P5/median/P95/max: **5 / 5.25 / 7.5 / 9.75 / 10 mm**.

Raise maximum to 10 mm: both faces become in range. With maximum 10, raise minimum
to 6 mm: only the small triangle is outside, giving **50% of faces, 20% of area**.
Limits always use longest edge, even when another heatmap metric is selected.

## 5. Topology and unavailable values

Six components occupy two rows. In world coordinates:

| Location | Component | Expected behavior |
| --- | --- | --- |
| X 0-1, Y 0-1 | Regular square, two triangles | Jump 1; four boundary edges. |
| X 3-4, Y 0-1 | Isolated triangle | Gray in jump mode; three boundary edges. |
| X 6-7, Y -1 to 1 | Three-triangle fan, one apex at Z=1 | One non-manifold edge; all three jump values unavailable. |
| X 0-1, Y 4-5 | Square with inconsistent winding | One winding edge; jump still 1. |
| X 3-4, Y 4-5 | Triangle duplicated with opposite winding | One duplicate; jump still 1, illustrating that quality alone does not establish validity. |
| X 6-8, Y 4 | Collinear face and repeated-vertex face | Two degenerate triangles, excluded from statistics. |

Totals: **12 triangles, 2 degenerate, 1 duplicate, 17 boundary edges,
1 non-manifold edge, and 1 winding edge**. Shape statistics count 10 valid faces;
jump statistics count 6, with 4 unavailable ratios. Degenerate faces have zero
area and cannot display a filled magenta region; confirm them using the counts.
Boundary edges are green, non-manifold edges pink, and winding edges red.

These surfaces are intentionally open. The last project is intentionally invalid;
none of the samples claims to certify FEM accuracy or a downstream volume mesh.

## Reproducing and verifying

`generate.py` deterministically regenerates the OBJ files, scenes, and `expected.json`
using Python's standard library. The JSON contains analytical reference values,
not numbers copied from Woby. The unit test **surface mesh quality sample projects
match analytical expectations** opens all projects through the real scene/OBJ
loaders and checks their values, limit percentages, and topology counts.

From the repository root:

```powershell
uv run assets/samples/surface-mesh-quality/generate.py
cmake --build --preset vs2026-vcpkg
ctest --preset vs2026-vcpkg -R "surface mesh quality sample projects"
```
