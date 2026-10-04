# Shader triangle edges

Opaque surfaces now shade triangle edges in their surface draw. Hidden edges
are suppressed by default. **View → X-ray triangle edges** restores edges
through surfaces. The equivalent control command is:

```text
woby ctl --instance ID render set scene --xray true
```

Use `false` for visible-surface edges. This is a scene setting, separate from
imported lines' existing depth-test control. PNG exports use the same draw plan
and shaders as the viewport.

## Rendering and storage

The source scene is submitted in these stages:

1. Depth-only occluders for opaque groups shown as hidden lines without fill.
2. Opaque surfaces, fusing enabled triangle edges when X-ray is off.
3. Transparent surfaces, preserving stable source order and separate blending.
4. Remaining triangle edges: shader edges for hidden lines, hardware lines for
   transparent groups and X-ray.
5. Depth-tested vertex circles.

Analyses and imported lines keep their subsequent passes. Transparent surfaces
do not write depth; their edges remain a separate blend, avoiding the fused
transparency appearance change observed in the prototype. Markers behind
transparent surfaces remain visible and selectable in the later marker pass;
opaque surfaces occlude them regardless of source group order.

Vulkan queries optional fragment barycentric support. Supported devices retain
indexed vertex reuse. Other devices, including the current Metal path, generate
barycentric coordinates by reading the original triangle indices in the vertex
shader. Native and fallback paths share edge shading, UV coloring, and picking
outputs. Native support is optional and does not raise the device requirement.

X-ray and transparent hardware lines also read the existing triangle indices,
emitting the same three segments per triangle. The viewer no longer requests or
uploads a separate triangle-edge index buffer. This removes 24 bytes per triangle
of GPU payload and the corresponding CPU construction/upload work when edges are
enabled: 1.69 GiB for BearTrap. This is an eliminated allocation, not a measured
device-wide VRAM delta. The legacy edge-buffer helper remains for comparisons.

`SceneDrawPlan` owns transforms, appearance, primitive metadata, and source
indices. Its runtime cache invalidates on scene replacement, geometry,
appearance, or visibility changes. Camera movement and selection do not rebuild
it. The cache contains no borrowed mesh pointers or graphics handles.

CPU object picking collects candidates once, then follows the source pass order.
Hidden-line interiors contribute depth without becoming selectable fills.
Marker drawing and packed provenance retain the existing circle shaders.

## Scene state

`UiState::triangleEdgeXray` is edited through `setTriangleEdgeXray`. It participates
in dirty tracking, undo/redo, and named views. Scene format 23 stores
`triangle_edge_xray` at the root and in each saved view. Missing values in older
scenes default to visible-surface rendering. New files require a reader that
supports format 23.

## Validation

The Debug viewer and tests build without compiler warnings. The full CTest run
covered 918 tests; 915 passed initially, and the three tests whose expectations
changed (default edge visibility, transparent marker picking, and the shader ABI)
passed after their expectations were updated and they were rerun. Focused tests cover
state persistence, defaults and invalid values, named views, undo/redo, cache
invalidation, CPU picking, and rendering at one and four samples with and without
the picking attachment. GPU comparisons exercise:

- Native and fallback shading, including triangle-index offsets above 65,535.
- Occlusion under group reordering and hidden-line depth before colored surfaces.
- Exact X-ray hardware-line coverage against the original indexed representation.
- Separate transparent surface/edge blending and source UV coloring.
- Vertex circles at 1, 4, 8, and 40 pixels.

All six new edge shaders also translate to Metal source without warnings. Metal
runtime validation requires macOS. Performance evidence that motivated the
integration is in the [prototype report](overlay-prototype-results.md); those
timings precede the production integration and retain legacy buffers.
