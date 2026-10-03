# Mesh memory lab

A native developer-facing prototype for following triangular mesh data from OBJ
records through CPU representations to actual GPU buffers. It uses woby's C++20,
SDL3, Dear ImGui, bx math, NoGraphicsAPI renderer, fonts, and compiled shaders.
The existing viewer is unchanged.

## Build and run

From the repository root on Windows:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_MESH_MEMORY_LAB=ON
cmake --build --preset vs2026-vcpkg
.\build\vs2026-vcpkg\bin\Debug\mesh_memory_lab.exe
```

The option defaults to `OFF`. The executable shares woby's staged assets and
runtime DLLs. Sample paths currently refer to this source checkout, so keep it
available when running the prototype. CI can enable the same option with the
existing `ninja-vcpkg` preset; no Visual Studio dependency is added to CI.

Select an example, drop a polygonal OBJ into the window, or use **Open OBJ** with
an absolute path. Loading happens on a worker; the current immutable capture
remains visible until the replacement is ready. Invalid input leaves it intact.

## Explore

- **File:** original source lines and file byte offsets. Click a record to follow
  its first use. Highlighting shows the records used by the selected triangle.
- **Triangulate:** separate double position, float normal, and float UV attribute
  arrays, original polygon provenance, and the resolved corner stream. OBJ's
  one-based and negative indices become zero-based indices; missing attributes
  are `-1`.
- **CPU layout:** the actual production `Mesh::vertices` and `Mesh::indices`.
  Vertex identity is the `(position, normal, texcoord)` index tuple. Select a
  vertex to see every render alias of its original source position.
- **GPU buffers:** real vertex/index handles, upload payload sizes, per-triangle
  index ranges, and byte-for-byte readback verification after completion.
- **Byte inspector:** the selected 32-byte `woby::Vertex`, field offsets, native
  memory-order hex bytes, float values and IEEE-754 fields. Position inspection
  includes the double original coordinate, origin, double local coordinate,
  float conversion, and conversion error.
- **Triangle assembly:** an actual indexed GPU draw with the selected triangle
  highlighted. Drag to orbit, use the wheel to zoom, and select the triangle and
  corner beside the viewport. Corner labels are explicitly an x-ray overlay and
  can show occluded vertices. They are projected from the same camera matrices.

The samples exercise a shared quad, UV seams, hard-normal seams on a cube,
concave polygon triangulation, and millimeter-scale offsets around coordinates
of magnitude `1e9`.

## What is measured

`loadTrace()` calls **the production `woby::loadObjMesh()`**. A separate RapidOBJ
parse reconstructs source provenance using the same localization and
triangulation policy. The tracer validates the resulting corner-to-vertex
correspondence, position correspondence, and source indices against the real
mesh. It checks the file again after capture to reject concurrent edits.

The attribute arrays displayed in the triangulation stage are **trace-owned
snapshots**, not retained allocations from the production parser. The host
address in the byte inspector is the address of the actual captured mesh vertex.
GPU handles are opaque; all GPU addresses shown are logical buffer offsets.

The GPU adapter uploads the captured vectors via `graphics::copy`, uses them in
the indexed draw, and reads both buffers back. It retains readback storage until
the renderer's completion frame. A new capture never replaces in-flight
readback storage. Uploads are transactional; a failed replacement keeps the
previous capture.

On this renderer the vertex shader pulls attributes from `root.vertices`:

```cpp
uint32_t vertex = indices[triangle * 3 + corner];
size_t byteOffset = vertex * 32 + component * 4;
// Shader equivalent: root.vertices[SV_VertexID * 8 + component]
```

`sizeof(Vertex) == 32`, position offset `0`, normal offset `12`, and UV offset
`24` are checked at compile time. UV V becomes `1 - sourceV`. If any render
normal is missing or invalid, production regenerates all render normals by
summing unit face normals and normalizing. Double local positions remain in
`Mesh::precisePositions`; they are not part of the GPU vertex payload.

Byte counts describe logical payloads, excluding vector spare capacity, vector
headers, parser scratch memory, driver allocation granularity and metadata.
The upload lifecycle is a description of the executed path, not a timing trace
or a report of physical GPU addresses.

## Validation

```powershell
ctest --preset vs2026-vcpkg -R mesh_memory_lab --output-on-failure
ctest --preset vs2026-vcpkg --output-on-failure
```

The CPU tests cover seam identity, triangulation, generated normals, concave
area preservation, large-coordinate localization, exact bytes, source line
offsets, negative indices, validation, and selection operations. Each file test
uses one unique system temporary directory and cleans it up on failure.
With `WOBY_TEST_HEADLESS=ON`, the GPU test uploads and reads back every sample,
checks camera projection, and verifies that the viewport rasterizes geometry.

For a reproducible headless view:

```powershell
.\build\vs2026-vcpkg\bin\Debug\mesh_memory_lab.exe `
  --sample hard-cube.obj --stage gpu --triangle 9 --corner 1 `
  --screenshot D:/woby/build/mesh-memory-lab.png
```

Other switches: `--stage source|corners|vertices|gpu`, `--width`, `--height`,
`--frames N` (exit after N native frames), and `--smoke` (GPU validation).

## Scope and structure

This is a local capture explorer, not a general process debugger or a live
attachment to another woby instance. It accepts face-based OBJ up to 512 KiB and
20,000 triangles; it explicitly rejects lines, points, freeform statements, and
continued records. Materials are parsed with woby's optional-material policy
but are not visualized. There is no scene editing or `.woby` persistence.

- `trace.*`: production capture, checked provenance, immutable data.
- `ui_state.h`, `ui_operations.cpp`: inspector selection and camera operations.
  This app owns inspector state only; it introduces no second editable woby
  scene or replacement for production `woby::UiState`.
- `gpu_runtime.*`: resources, uploads, completion, camera matrices and rendering.
- `ui.*`: ImGui views, editing local values through inspector operations.
- `main.cpp`: SDL runtime, worker publication, lifecycle, smoke and screenshot modes.

The next useful extension is a reusable capture schema for typed allocations,
byte ranges, identities, and transformation edges, so other pipelines can reuse
the inspector without embedding mesh-specific assumptions in every view.
