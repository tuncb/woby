# Render-path allocation audit

Audit of the application frame loop, scene submission, overlays, ImGui adapter,
and their CPU preparation functions (September 2026). This is a source audit
with targeted allocation measurements, not a claim that the whole frame or the
graphics driver performs no allocations.

## Changes

| Path | Previous recurring work | Implementation |
| --- | --- | --- |
| Grid | Heap vector, followed by a copy to transient vertices | Write directly into bgfx's transient vertex buffer. |
| Origin axes | Heap vector for two vertices | Two stack vertices. |
| Selection | New part list, new outline vector, another vector converting identical position data | Retain part/outline capacity in `SceneRenderScratch`; submit positions without the extra conversion. |
| Dimensions | New part list in the inspector and new cache-key vector even on a cache hit | Retain inspector part capacity; compare/update cache keys in place. |
| Scale labels | Growing vector of collision rectangles | Stack array of seven: one grid label, up to three readout labels, three edge labels. |
| Annotation strokes | Per-frame part list; per-annotation source lists, transforms, world lines, expanded vertices | Reuse runtime vectors, resolve sources once per annotation, return before stroke work when hidden/unattached. |
| Diagnostic focus | New point/fill/expanded-triangle vectors | Reuse runtime point and position buffers; expand into the fill buffer. |
| Object rows | Name/badge concatenation, tooltip construction, ID formatting strings | Draw badge/name separately; format bounded IDs on the stack; construct tooltips only on hover. |
| Window title | Build strings and call SDL twice per unchanged frame | Update only when path/dirty state changes. |
| Object lookup | Materialize owning names/paths for every visited object before testing its ID | Filter IDs first; only materialize the result. |
| Scene bounds without analyses | Build a corner vector that is never used | Return after the existing source-bounds calculation. |

Scratch belongs to the viewport/export runtime, outside `UiState`. It retains
the largest capacity needed so far and is released with that runtime. Contents
are rebuilt on each use, so transforms, visibility, selection, replaced meshes,
and recomputed comparison geometry cannot leave a stale cached result. Borrowed
part/source pointers are cleared or overwritten before access. Viewport and
screenshot scratch are independent. Submission copies vertices into bgfx-owned
transient memory before CPU scratch is reused.

## Other allocation sites and lifetime decisions

| Path | Finding / decision |
| --- | --- |
| `submitSceneFiles`, hover signature and vertex picking | No application heap containers created in ordinary submission/traversal; matrices/colors are stack values and mesh buffers persist. |
| `createGpuMesh`, `prepareGpuMeshFeatures` | Mesh, edge, and point-sprite vectors/uploads allocate on load or first demand, not each draw. Keep owned storage: `bgfx::makeRef` release callbacks may run on the render thread after this frame. |
| Comparison result uploads | Geometry/diagnostic vectors and `bgfx::copy` run when result stages or visualization data change. GPU buffers persist between submissions. |
| Comparison readiness, signatures, and bounds | Member-ID vectors, deduplication sets, and bounds corners still allocate during frame preparation. They also serve scene operations. Caching these needs careful invalidation of membership, transforms, and geometry; it is separate from the scratch-only changes here. |
| Inspector property queries | Target lists/maps/sets and owning metadata still allocate. Filtering object lookup avoids copies for unrelated objects. A future aggregate query could resolve all displayed properties in one traversal; a blanket allocator substitution would not remove the repeated traversal. |
| Comparison input trees and UI text | `comparisonTree` creates owning trees, names, member lists and deduplication storage. Inspector, annotation, saved-view, detector and legend labels also create strings. These remain panel-dependent allocations. |
| Annotation handles / gestures / click picking | Handle projection is evaluated after navigation settles; gesture projection and click picking own separate geometry with longer lifetimes. These are not per-draw stroke scratch. |
| ImGui | Draw lists and internal widget storage retain their own capacity. Growth, new windows/widgets, font glyph/atlas creation, and atlas uploads can allocate. `imgui_bgfx::renderToView` uses bgfx transient vertex/index buffers; texture copies occur on texture creation/update. |
| Screenshots | Framebuffers, readback pixels, export text/draw lists and PNG encoding allocate on a capture request. Export submission now also uses runtime scratch. |
| History, background work, dialogs, automation, logging | Scene snapshots allocate on edits, workers on jobs, dialogs/commands on requests, logging when enabled/emitted. Their objects often outlive a frame and cannot borrow frame-arena storage. |

## Why no general frame arena

bgfx already supplies an arena-like transient buffer: its frame allocator
advances an offset in preallocated vertex/index storage. Use that directly when
the output size is known, as for the grid. For clipped annotation strokes and
variable selection/focus geometry, retained vectors remove repeat allocation
without changing standard-container APIs or introducing arena exhaustion and
cross-frame lifetime rules. Small bounded data fits on the stack.

A `std::pmr::monotonic_buffer_resource` could help the remaining temporary UI
trees, but would require allocator-aware nested strings/containers and explicit
handling of overflow storage. Merely constructing and destroying an arena each
frame would still allocate whenever its initial backing buffer was exceeded.
Persistent asynchronous uploads, history, and logical scene data must not share
such an arena.

## Verification

MSVC Debug CRT allocation probes run the warmed CPU preparation workloads 100
times each and require **zero allocations** for selection parts/outlines,
multi-source annotation world lines, and unchanged dimension keys. Tests also
exercise hiding/restoring sources, missing source IDs, transformed endpoints,
shrinking selections, and replacing scene geometry. Metadata misses and bounds
updates without analyses have a separate zero-allocation check. Capacity and
behavior checks run on other platforms; CRT allocation counting is Windows
Debug only. Assertions execute outside the measured scopes.

The debug build uses `cmake --build --preset vs2026-vcpkg`. The full CTest suite
and `uv run tests/ctl_annotation_render_smoke.py
build/vs2026-vcpkg/bin/Debug/woby.exe` cover behavior and rendered annotation
alignment, including large translations, both up axes, and different near planes.
These measurements establish reduced allocation churn in the named paths.
The subsequent [before/after performance measurements](render-allocation-performance.md)
report Release CPU timings and whole-frame Debug allocation counts, including
the small annotation timing regression and the effect of VSync on frame delivery.
