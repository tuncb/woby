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
internal face across cell groups only when exactly two incident faces have
known, opposite orientations. Faces must use the same vertex indexes and the
same mid-edge node for each edge. Source point identity metadata does not join
cells. Linear and quadratic faces do not match each other.

For display and surface analysis, Woby divides each six-node boundary face
into four flat triangles. All mid-edge positions affect these triangles.
This is a fixed approximation of the curved quadratic surface. Woby does
not evaluate a curved finite element surface or render interior cells.

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
