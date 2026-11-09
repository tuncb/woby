# VTK tetrahedral meshes

Woby reads legacy ASCII `.vtk` files with `DATASET UNSTRUCTURED_GRID`.
No VTK library is required. Open a file from the model dialog, drop it into
Woby, or use `--file`. Folder scans include `.vtk` files. Saved `.woby`
scenes reload the source file and restore the group settings.

Supported versions are 2.0, 3.0, 4.2, and 5.1. Version 5.1 uses `OFFSETS`
and `CONNECTIVITY` records. Point coordinates can use `float` or `double`.
Woby keeps double precision before it chooses the working coordinate origin.

| VTK cell type | ID | Display |
| --- | --- | --- |
| Vertex / polyvertex | 1 / 2 | Points |
| Line / polyline | 3 / 4 | Line segments |
| Triangle | 5 | One triangle |
| Quad | 9 | Two triangles |
| Tetrahedron | 10 | Outer triangular faces |
| Quadratic triangle | 22 | Four triangles |
| Quadratic tetrahedron | 24 | Four triangles per outer face |

The reader accepts mixed supported cell types. Woby creates groups named
`Mesh`, `Lines`, `Points`, and `Tetrahedra` as needed. It ignores attribute
arrays after `POINT_DATA`, `CELL_DATA`, or a trailing `FIELD` record.
Attribute arrays do not set colors or groups.

Binary VTK, XML `.vtu`, structured datasets, and other cell types are not
supported. Convert these files to a supported ASCII unstructured grid.
Unsupported cells or invalid data buffers produce an error. The reader does not
skip unsupported cells. Dataset field records before the geometry are
not supported.

## Quadratic tetrahedra

The node order matches
[VTK quadratic tetrahedra](https://vtk.org/doc/nightly/html/classvtkQuadraticTetra.html):

| Node | Location |
| --- | --- |
| 0, 1, 2, 3 | Corners |
| 4 | Edge 0–1 |
| 5 | Edge 1–2 |
| 6 | Edge 2–0 |
| 7 | Edge 0–3 |
| 8 | Edge 1–3 |
| 9 | Edge 2–3 |

Woby keeps the source coordinates and cell connectivity. It removes a shared
internal face within a cell group only when exactly two incident faces have
known, opposite orientations. Faces must use the same vertex indexes and the
same mid-edge node for each edge. Source point identity metadata does not join
cells. Linear and quadratic faces do not match each other.

Each cell group keeps its complete boundary. Shared faces between groups stay
in the mesh so that a group stays closed when a neighboring group moves or is
hidden. For topology checks on one volume, select that group. A check on several
touching groups includes their coincident interface faces.

For display and surface analysis, Woby divides each six-node boundary face
into four flat triangles. All mid-edge positions affect these triangles.
This is a fixed approximation of the curved quadratic surface. Normal model
view shows the boundary. Tetrahedral quality view can show interior cells.
Neither view renders an exact curved finite element surface.

Woby uses the quadratic mapping's Jacobian at the reference-cell center to
set quadratic-cell winding. Linear cells use the corner determinant. These
checks set display orientation; they do not validate the entire cell.
Reversed connectivity is accepted. If the determinant is zero or nonfinite,
or node indexes repeat, Woby keeps source winding and all affected faces.
Duplicate cells, conflicting face orientations, and faces with more than two
incident cells are retained for inspection. Node references must remain in
range, and coordinates must be finite and within the supported limits.

The sample [quadratic-tetra.vtk](../assets/samples/quadratic-tetra.vtk) contains
one quadratic tetrahedron with a displaced mid-edge node.

## Tetrahedral cell quality

Create a **Mesh quality** analysis. Select **Tetrahedral cells** in its metric
controls. The analysis includes all cells in the selected source parts,
including cells and nodes that do not occur on the exterior. Measurements use
the analysis placement and double-precision positions.

| Measure | Meaning |
| --- | --- |
| Signed volume | Integral of the Jacobian over the cell. All 10 nodes affect a quadratic cell. A folded cell can have a positive total signed volume. |
| Corner scaled Jacobian | Signed determinant, normalized by the largest product of three incident corner-edge lengths and scaled by `sqrt(2)`. A regular positive tetrahedron has value 1. |
| Corner mean ratio | `12 * (3 * abs(corner volume))^(2/3) / sum(squared corner edges)`. A regular tetrahedron has value 1; a flat cell approaches 0. |
| Minimum/maximum corner dihedral | Smallest/largest angle between the four corner faces, in degrees. A regular tetrahedron has angles of approximately 70.53 degrees. |
| Normalized Jacobian lower/upper bound | Bounds over the whole reference cell. Divide the determinant by the cube of the longest distance between cell nodes. |
| Jacobian ratio lower bound | Conservative lower bound on minimum/maximum Jacobian after correction for consistent reversed order. This describes variation within a curved cell. |

Corner shape and angle measures are approximations for quadratic cells. Use
the validity result to check the curved mapping. A ratio of 1 for a linear
cell describes its constant Jacobian; it does not establish good cell shape.

### Validity results

The quadratic check uses cubic Bernstein bounds with adaptive subdivision
and outward-rounded arithmetic. It checks the complete reference cell;
it does not use only the center or a fixed set of sample points.

- **positive:** the Jacobian stays above the normalized tolerance.
- **reversed:** the Jacobian stays below the negative tolerance. The cell
  has consistent reversed node order. This result is separate from folding.
- **folded:** the check found both positive and negative Jacobians.
- **collapsed:** the entire bounded determinant is within `±1e-12` after
  cell size normalization.
- **unresolved:** the check could not prove a sign. This includes some
  near-singular cells and cells that reach the work limit. It is not a pass.
- **repeated_nodes:** at least two local nodes use the same vertex index.

These results describe each cell's mapping. They do not check overlaps
between cells or establish accuracy for a particular solver.

The histogram and percentiles include all finite values, including values
from invalid cells. Unavailable values are omitted. Minimum and maximum
limits count values strictly outside their inclusive endpoints. These limits
do not change the validity result or heatmap colors.

Select a row to frame its cell. Enable **Selected cell only** to inspect an
interior cell. **Full result** clears the selection. Magenta identifies
folded, collapsed, or repeated-node cells. Gray identifies unresolved or
unavailable values. Cell selection is temporary; metric, limits, and display
settings are saved in `.woby` scenes.

### Automation and export

Use `analysis set ID --mode surface_quality --tetrahedral true` to select
cell quality. Set `--tetrahedral-metric` to `volume`, `scaled_jacobian`,
`mean_ratio`, `minimum_dihedral`, `maximum_dihedral`, `jacobian_lower`,
`jacobian_upper`, or `jacobian_ratio`.

`analysis results ID` returns `tetrahedralMeshQuality` metadata on each side.
Page through cells with:

```text
analysis findings ID --side a --detector tetrahedral_quality --collection /cells
analysis focus ID --side a --detector tetrahedral_quality --index 1
analysis set ID --tetrahedral-selected-only true
```

The result index and `sourceCell` are one-based. `sourceCell` is the original
VTK cell number, including other cell types in a mixed file. For importer
plugins, it is the position in the plugin's tetrahedron array. It is not an
authored solver ID from `CELL_DATA`. `partId` identifies the source part.
Full JSON exports include every measured cell in `tetrahedralCells`.
