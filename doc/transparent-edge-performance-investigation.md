# Transparent surface and triangle edge performance

The follow-up [prototype measurements](transparent-edge-performance-results.md)
now compare the combined policies, native/fallback paths, CPU cost, and memory.
The figures and unmeasured proposals below record the initial assessment.

Combining transparent surfaces and triangle edges in one geometry pass is the
best first optimization to evaluate for [issue 118](https://github.com/tuncb/woby/issues/118).
It removes the very expensive procedural hardware-line draw while retaining the
original triangles. The choice of edge opacity primarily determines appearance;
it does not require choosing between one and two geometry passes.

Start with edges that inherit surface opacity, then compare an independent edge
opacity in the same shader. Retain the existing overlay for X-ray and modes that
require hardware-line coverage. A separate edge image written during the same
triangle draw is a useful third candidate when overlay prominence matters more
than minimum bandwidth.

This investigation covers the renderer at `2723084` on 7 October 2026. Its
rendering code is unchanged from the issue's measured revision `ac2d82e`.
Performance figures below are the existing measurements, independently checked
against their retained raw samples. Proposed fused transparent variants have
not been benchmarked; their expected benefits are engineering inferences.

## Current rendering and measured cost

`submitSceneFiles` establishes opaque depth, draws transparent surfaces into
weighted blended order-independent transparency (OIT) buffers, resolves those
buffers, draws remaining triangle edges, and finally draws opaque vertices and
point clouds. Transparent surfaces and triangle edges do not write depth.

Opaque solid meshes with ordinary edges already use barycentric shading in one
triangle draw. Transparent edges use `vs_triangle_lines`, which emits six
vertices and three hardware lines per triangle by reading the original triangle
indices. Shared edges are emitted repeatedly. There is no dedicated edge-index
buffer in this production path.

Issue 118's measurements use Release Vulkan, an RTX 3070 Laptop GPU, 4x MSAA,
a 1280 by 720 drawable with a 1280 by 687 scene viewport, fitted isometric
cameras, 35 percent surface opacity, and no vertex rendering. Each value is
the median of three per-round GPU frame medians. Timings include scene, UI,
transparency resolve, and presentation composite work; they are not displayed
FPS.

| Mesh | Triangles | Surfaces only | Surfaces and transparent edges | Surfaces and opaque edges |
| --- | ---: | ---: | ---: | ---: |
| BusGameMap | 1.05 million | 1.048 ms | 1.915 ms | 1.306 ms |
| Bennu | 17.87 million | 4.783 ms | 17.239 ms | 15.666 ms |
| BearTrap | 75.77 million | 15.812 ms | 143.539 ms | 143.912 ms |

Enabling edges adds approximately 0.867, 12.455, and 127.727 ms respectively
relative to the corresponding surface-only condition. These differences are
screening estimates from whole-frame medians, not isolated pass timestamps.
For BearTrap the increment accounts for approximately 89 percent of the combined
frame. Its edge-only condition independently costs 128.706 ms, reinforcing the
case for removing the line workload.

BearTrap emits 454,631,916 procedural line vertices per frame. Making those
vertices opaque does not remove their vertex processing, primitive setup,
rasterization, or repeated shared edges. Its paired opaque-edge savings range
from -0.373 to +0.454 ms, without a consistent improvement. This supports a
geometry/rasterization investigation ahead of blend-state tuning; it does not
identify a specific hardware unit without GPU profiling.

CPU scene submission is already small for these large single-group models:
BearTrap measures approximately 0.0053 ms for surfaces and 0.0099 ms with edges.
Removing one draw command alone cannot explain a roughly 128 ms GPU saving.
Reducing submitted geometry is the important change. Scenes with thousands of
groups can also benefit from fewer submissions.

The [earlier opaque prototype](overlay-prototype-results.md) measured BearTrap
at 125.95 ms with ordered surfaces and lines, 15.66 ms with native fused edges,
and 20.07 ms with the pulled fallback. This demonstrates that the existing
barycentric approach removes the expensive representation. Transparent content
retains more contributing layers and blending work, so those figures do not
predict its fused timing.

The current transparent combined/surface-only ratios are 1.83, 3.60, and 9.08.
They describe the available performance opportunity if edge overhead became
negligible. They are not measured fused speedups or guaranteed targets.

## What one pass means

The proposed optimization uses one triangle draw per transparent solid group
to produce both fill and edge contributions. It still needs opaque visibility,
transparency accumulation, a screen-space resolve, and later point rendering.
Different groups still have their own transforms and appearance. This is not
one draw call or one render pass for the entire scene.

The triangle fragment shader already shades every contributing surface
fragment. Barycentric edge coverage adds local arithmetic to those invocations
instead of launching a second geometry workload. Native Vulkan barycentrics
retain indexed vertex reuse. The existing portable fallback generates three
triangle corners with vertex pulling; it loses indexed reuse but eliminates
the additional six line vertices per triangle.

The current shader uses screen-space derivatives of nonperspective barycentric
coordinates to estimate distance to the nearest triangle edge. Its edge color
is the group RGB multiplied by 1.25 and clamped. Surface lighting and UV coloring
remain available underneath.

This is likely the most economical approach for solid-plus-edge dense meshes.
It is not universally faster for every mode: the shader does additional work
throughout the triangles, while sparse line-only drawing can avoid shading large
interiors. X-ray and exact centered-line silhouettes also need different coverage
or depth behavior.

## Appearance options

| Option | Geometry for transparent solids with edges | Appearance | Implementation scope |
| --- | --- | --- | --- |
| Match surface opacity | One triangle draw | Edges are part of the translucent surface; foreground surfaces attenuate deeper edges through approximate OIT | Smallest change; existing two transparency targets |
| Independent edge opacity | One triangle draw | Stronger or weaker edges with a controllable local opacity | Same targets; shader parameters and persistent scene controls |
| Opaque edges in the OIT shader | One triangle draw | Strong edge coverage, but overlapping colors can still mix | Same targets; must describe the OIT limitation accurately |
| Separate overlay image from the triangle shader | One triangle draw writing a third color target | Edges composite after resolved surfaces and remain prominent | Broader adaptor changes and extra image bandwidth |
| Existing hardware-line overlay | Surface draw plus line draw | Current coverage and layering, including centered silhouettes | Compatibility path; dense-mesh cost remains |

### Edges that match surface opacity

Let `a` be inherited surface opacity, `c` analytic edge coverage, `S` shaded
surface RGB, and `E` edge RGB. The simplest fused material is:

```text
alpha = a
rgb   = (1 - c) * S + c * E
```

Feed this material into the existing OIT accumulation. An isolated sheet at
35 percent opacity stays at 35 percent across both its fill and edges. This
creates a coherent transparent object and avoids adding opacity solely because
edges are enabled. It is the best first candidate for a default.

This is an intentional appearance change from the existing overlay. A surface
and a fully covered edge each blended at 35 percent have combined alpha
`1 - (1 - .35)^2 = .5775`, or 57.75 percent. If the same hardware edge blends
twice, the surface plus two edge contributions can reach 72.54 percent. The
exact existing pixel depends on coverage and overlap; these examples explain
why equal numeric edge and surface opacity does not currently produce equal
visual transparency.

### Independent edge opacity in the same pass

Treat the edge as a local coating over its own surface. With edge opacity `e`
and `x = e * c`, compose the local material in premultiplied form:

```text
alpha       = x + (1 - x) * a
premul_rgb  = x * E + (1 - x) * a * S
rgb         = premul_rgb / max(alpha, epsilon)
```

This handles edge antialiasing without dark fringes. Setting `e = a` gives
57.75 percent alpha at a fully covered edge on a 35 percent surface. Setting
`e = 1` makes a fully covered local edge opaque while leaving the interior at
the surface opacity. The geometry and render targets are unchanged; the extra
arithmetic should be minor, but needs measurement.

This local composition does not reproduce a globally later overlay. A front
transparent sheet can affect a deeper edge's contribution. The existing overlay
draws after all surfaces and therefore remains prominent even when the edge is
behind a transparent surface.

The current weight function saturates its alpha factor above roughly 0.1.
Increasing edge alpha therefore does not automatically give its color an
overwhelmingly larger weight. Any proposed additional edge-color weighting needs
its own visual review and numerical checks.

### Fully opaque edges and overlapping surfaces

Weighted OIT computes accumulated weighted colors and multiplicative
transmittance. A contribution with alpha one makes final transmittance zero,
but it does not remove farther colors from the weighted average. For example,
with equal weights, a red alpha-one edge and a blue alpha-0.35 layer produce
approximately 74 percent red and 26 percent blue, despite full final coverage.
The average is independent of which color is in front.

Consequently, an opaque-edge slider inside this accumulator means opaque local
coverage, not guaranteed nearest-edge occlusion. Weighted OIT deliberately
approximates color ordering, as explained by
[McGuire and Bavoil](https://jcgt.org/published/0002/02/09/) and
[McGuire's implementation notes](https://casual-effects.blogspot.com/2015/03/implemented-weighted-blended-order.html).

If the desired behavior is that the nearest opaque edge hides all farther
geometry, use a depth-aware hybrid rather than expecting an alpha change to do
that. An edge visibility/depth pass followed by transparent accumulation is one
candidate. It needs separate handling of partial coverage and must preserve the
accepted policy that points remain prominent through transparent surfaces.
This is a larger rendering change than issue 118's primary optimization.

### Overlay appearance with one geometry pass

One triangle fragment invocation can write surface accumulation, surface
revealage, and a separate edge color/alpha image using independent attachment
blending. The existing screen-space resolve can combine surfaces first and
then the edge image. This still removes the full line draw; an additional
screen-space pass is not inherently required.

Conventional source-over blending into the edge image can retain the current
source-order overlay policy for the new triangle coverage. It also retains the
current order dependence at intersections between differently colored edges.
An OIT edge image could make edge order independent, but would require another
revealage target and would retain OIT's color approximation.

Mixed X-ray or edge-only draws need to enter the same overlay image in their
intended order, or be composited in explicitly ordered batches. Resolving the
new edge image before every remaining overlay would otherwise change their
relative layering.

The adaptor currently supports at most two color attachments. This option needs
a third attachment, its own blend and write-mask settings, pipeline cache keys,
clear values, shader texture bindings, and multisample reads. One 1280 by 720
RGBA8 attachment at four samples alone consumes approximately 14.1 MiB, before
resolve images, allocation overhead, and copies for frames in flight. Every
contributing triangle may also incur additional attachment traffic, even away
from an edge. These costs matter on bandwidth-limited and tile-based devices.

This is the strongest alternative if prominent overlay edges are the desired
product behavior. It should be benchmarked against the simpler material approach
before accepting the larger adaptor change. It cannot exactly reproduce
hardware-line pixels: the geometry coverage is still triangle based.

## Coverage and modes that need special handling

Barycentric edges lie inside their triangles. They cannot shade the outside
half of a centered hardware line at a silhouette. Tiny triangles can become
almost entirely edge colored; the retained Bennu and BusGameMap captures already
show how dense line overlays can obscure surface detail. Line thickness, color
contrast, and behavior when zoomed out deserve separate choices from opacity.
The [original wireframe paper](https://www2.imm.dtu.dk/pubdb/edoc/imm4884.pdf)
also describes the silhouette coverage limitation.

Start with the existing derivative-based antialiasing. Compare 1x and 4x MSAA,
slanted edges, shared boundaries, silhouettes, clipped triangles, and subpixel
triangles. Per-sample shader evaluation may improve some boundaries but can
increase cost substantially; do not enable it globally without evidence.

Explicit X-ray can require edges to ignore opaque depth while surfaces respect
it. A normal fixed-function depth test in one draw cannot express both policies.
Retaining the current X-ray overlay avoids a more expensive manual-depth design.
Opaque edge-only hidden-line rendering already uses an occluder prepass and a
shader edge draw. Transparent edge-only rendering should be evaluated separately;
changing its visibility policy is not necessary to optimize solid-plus-edge.

Keep imported lines, detector overlays, vertices, and point clouds on their
existing paths. [Issues 115](https://github.com/tuncb/woby/issues/115) and
[116](https://github.com/tuncb/woby/issues/116) establish that points stay opaque,
prominent, and selectable through transparent surfaces. Fused transparent
surfaces must continue testing opaque depth without changing it or the point-ID
attachment.

## Implementation and validation priorities

1. Share transparency accumulation between ordinary and fused edge shaders.
   Add native and pulled programs to `TransparentSurfacePrograms`. Route eligible
   solid-plus-edge, non-X-ray groups through them and omit only their later line
   draw. Use the same resolve programs for all transparent groups so they remain
   in one accumulated batch. Keep surface-only colors unchanged.
2. Compare matched surface opacity and the local coating formula. Preserve
   inherited opacity, UV shading, source indices, and the no-depth-write policy.
   If an edge appearance control is added, define whether zero surface opacity
   hides edges or leaves an independently visible wireframe. Store that decision
   in `UiState`, edit through `ui_operations`, and map it through scene files,
   saved views, undo/redo, and draw-cache invalidation.
3. Evaluate the separate overlay image if the two material variants lose desired
   edge prominence. Measure its attachment memory and bandwidth rather than
   assuming one geometry pass makes every variant equally cheap.
4. Optimize remaining overlays only after the fused path is measured. Shared-edge
   deduplication could roughly halve line count on a manifold mesh, but seams,
   appearance ownership, boundaries, and nonmanifold edges complicate it. It also
   changes repeated-edge opacity and can reintroduce substantial geometry memory.
   Cache any topology work and run expensive construction off the UI thread.

Benchmark surface-only, legacy overlay, both material variants, the overlay-image
variant if implemented, and native/pulled paths. Use the three existing real
meshes plus a scene with many groups. Include fitted, close, and distant views,
1x/4x MSAA, several opacities, offset viewports, and mixed opaque/transparent
objects. Rotate order over independent rounds and record per-pass GPU timing,
CPU submission, memory, geometry counts, and images. Avoid changing triangle
visibility or resolution to manufacture a speedup. Full-detail screenshot
exports need their own coverage checks because their resolution differs from
the timed viewport.

GPU regressions should check known single-layer colors, independently colored
intersections under group and triangle reorder, opaque occlusion, ordinary and
reversed depth, UV coloring, zero opacity, opaque transitions, untouched marker
IDs, and the chosen edge appearance at 1x/4x MSAA. Existing CPU object picking
mirrors surface/edge/point pass order, so visual compositing changes need an
explicit selection-policy check as well. Compile Vulkan and Metal shaders;
Metal runtime claims require a macOS run.

The investigation reran the four existing focused GPU cases: all four and all
113 assertions passed. Retained raw measurement hashes and per-round medians
were independently verified, with zero point submissions in measured conditions
and no campaign guard violations. No rendering implementation changed during
this investigation.

Relevant implementation files are `src/scene_renderer.cpp`,
`shaders/native/woby.slang`, `src/graphics.cpp`, `src/scene_draw_plan.cpp`, and
`src/scene_pick.cpp`. Current GPU regression coverage is in
`tests/scene_renderer_gpu_tests.cpp` and `tests/scene_edge_gpu_tests.cpp`.
