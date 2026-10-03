# Mesh memory lab

A native prototype for inspecting data shapes and transformations in woby's
triangle-mesh pipeline. Uses C++20, SDL3, Dear ImGui, bx math, NoGraphicsAPI,
woby's fonts, production mesh construction, and production shaders.

The app contains one embedded OBJ: two quads folded along a shared edge, with
independent UV and normal domains on either side of the seam. Six original
positions become four triangles and eight render vertices. Coordinates near
`1e9` make the double-to-float localization visible. No file selection or source
checkout is needed at runtime.

## Build and run

From the repository root on Windows:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_MESH_MEMORY_LAB=ON
cmake --build --preset vs2026-vcpkg
.\build\vs2026-vcpkg\bin\Debug\mesh_memory_lab.exe
```

The opt-in target shares woby's staged assets and DLLs. It launches as a native
GUI without opening a console; redirected command-line output still works.
CI can enable the same option with the existing `ninja-vcpkg` preset.

## Explore the data train

The main view alternates data blocks with transformers:

```text
OBJ text -> Parse -> Attribute pools -> Rebase + split -> Corner stream
         -> Intern + pack -> CPU mesh -> Copy to GPU -> GPU buffers
```

Click any block to open its inspector. Click source lines, attribute records,
corners, vertices, or index rows to follow their identity through the train.
The right-hand inspector links the selected triangle corner to its source
position, normal and UV indices, packed vertex, field bytes, and GPU offset.

- **OBJ text / Parse:** exact embedded text, line and byte offsets, destination
  arrays, decimal parsing, and conversion to independent zero-based indices.
- **Attribute pools:** every original double position, float normal and UV,
  original polygon corner tuples, and the render aliases of each position.
- **Rebase + split:** original and localized doubles, origin calculation,
  the quad diagonal rule, and each original polygon's output triangles.
- **Corner stream / Intern + pack:** all twelve corner tuples and their output
  IDs, including each first insertion and subsequent reuse. Shows float casts,
  authored normal copying, UV V inversion, and seam-induced vertex splits.
- **CPU mesh:** exact 32-byte vertex records, offsets, alignment, and retained
  double/source-identity arrays. CPU payload totals 688 bytes.
- **Copy to GPU:** allocations, ownership, upload calls, completion, and the
  distinction between retaining the 688-byte CPU payload and copying only
  304 bytes into GPU buffers.
- **GPU buffers:** actual handles, index values, vertex-fetch addresses and
  the production vertex shader's word addressing. Both buffers are read back
  and compared byte for byte after completion.

Click a packed field on the right to inspect its float32 value, native-order
hex bytes, sign/exponent/fraction bits, returned GPU bytes, and conversion from
the source value. Position fields show the double original, origin, localized
double, float conversion error, and the result of casting without rebasing.
Alias buttons reveal both render vertices at a seam position.

The small viewport draws the actual buffers through production mesh shaders.
Drag to orbit and scroll to zoom. Triangle/corner controls update the linked
selection. Projected labels and outlines are x-ray overlays, including occluded
corners; shader invocation order and cache behavior are not inferred.

## Measurement and scope

`internalExample()` calls `woby::loadObjMeshText()`, which uses the same mesh
construction as the file importer. A second RapidOBJ parse reconstructs
provenance using the same localization and triangulation policy. The trace
validates the corner-to-vertex and source-position correspondence against the
production result. Intermediate arrays are trace-owned snapshots, not retained
parser allocations or observations of another running woby process.

The data payloads are 328 bytes for the parsed attributes and original corner
tuples, 376 bytes after triangulation, 688 bytes for the retained CPU arrays,
and 304 bytes for GPU vertices and indices. Payload counts exclude container
headers, spare capacity, provenance metadata, parser scratch memory, and driver
allocation granularity. GPU handles are opaque; addresses shown are resource
offsets. The transfer description follows executed calls, not sampled timings.

`sizeof(Vertex) == 32`, position offset `0`, normal offset `12`, and UV offset
`24` are checked at compile time. The shader reads
`root.vertices[SV_VertexID * 8 + component]`. Readback storage remains owned
until the renderer reports completion. Scene editing and `.woby` persistence
are outside this inspector's scope.

## Validation

```powershell
ctest --preset vs2026-vcpkg -R mesh_memory_lab --output-on-failure
ctest --preset vs2026-vcpkg --output-on-failure
```

CPU tests cover the internal example, payload accounting, transformation
endpoints, vertex insertion/reuse, selection, file/memory loader parity,
triangulation, seam identity, generated normals, large coordinates, byte layouts,
and invalid input. Existing OBJ files under `samples/` are regression fixtures
only. File tests use unique temporary directories with cleanup on failure.
With `WOBY_TEST_HEADLESS=ON`, the GPU test uploads and reads back the built-in
example, checks projection, and verifies rasterization. The Windows launch test
checks GUI subsystem behavior and redirected diagnostics.

For a headless screenshot:

```powershell
.\build\vs2026-vcpkg\bin\Debug\mesh_memory_lab.exe `
  --node pack --triangle 2 --corner 0 `
  --screenshot D:/woby/build/mesh-memory-lab.png
```

`--node` accepts `source|parse|attributes|triangulate|corners|pack|mesh|upload|gpu`.
Other switches: `--width`, `--height`, `--frames N` (exit after N native frames),
and `--smoke` (GPU validation).

## Structure

- `example.cpp`: embedded source.
- `trace.*`: immutable production capture and checked provenance.
- `pipeline.*`: data/transform topology and payload accounting.
- `ui_state.h`, `ui_operations.cpp`: inspector selection and camera operations.
  This is inspector state; it does not replace production `woby::UiState`.
- `gpu_runtime.*`: resources, upload/readback, camera matrices and rendering.
- `ui.*`: train, data tables, transformer details and linked byte inspector.
- `main.cpp`: SDL lifecycle, command-line, smoke and screenshot modes.
