# Shared spatial code investigation

Decision: share small box and ownership utilities when this reduces duplicate code. Keep the tree layouts and query algorithms separate. There is no measured performance reason to replace them with one tree.

This audit applies to commit `46a27585c5bc8638a8bde5d601e5cead71376631` on 11 October 2026. This report uses working item number 17 for the unnumbered shared spatial infrastructure entry in the source assessment.

## Current contracts

| Path | Input and order | Query | Required retained data |
| --- | --- | --- | --- |
| Distance | Double triangle coordinates; tree construction reorders triangles | Nearest distance with a previous-face hint | Four-child double boxes in SIMD lanes; contiguous leaf triangles; 100-entry stack |
| Self-intersection | Source faces with stable report IDs; separate box/order arrays | Enumerate overlapping face pairs within a source batch | Double boxes, source-order lookup, work/output budgets and deterministic finding order |
| Point cloud | Float positions with original marker IDs; Morton order | Screen footprint, picking, view detail and navigation | Per-group roots, proxy ranges, six extrema and prepared navigation cuts |
| Annotation mesh | Float render geometry with original triangle offsets | Cull source triangles for a projection gesture | Triangle-order map, source index ranges and an immutable geometry owner |
| Annotation projection | Camera-clipped triangles in 2D screen coordinates | Gesture region tests and surface attachment | Double screen boxes, original face IDs and barycentric clipping records |

Sources: [distance tree](../src/mesh_comparison.cpp), [intersection tree](../src/mesh_intersections.cpp), [point cloud](../src/point_cloud.h), [annotation mesh](../src/model_mesh.h), [projection cache](../src/surface_annotation.h).

The distance builder removes its permutation array after it moves triangles into leaf order. The intersection path needs its permutation to recover source faces. A shared leaf representation would put an extra lookup back into the distance path or require another source-ID array. Neither is a code deletion with a free performance gain.

The point tree does more than select nearby points. Its proxy ranges and extrema preserve source coverage during navigation. A generic nearest-point tree does not meet that contract. Screen-space annotation boxes have two dimensions and change with the camera. They cannot share a cached world tree.

## Storage consequences

The [instance query probe](../experiments/simplification-13/main.cpp) records the compiled x64 node sizes in its JSON. The intersection record is 80 bytes: a 48-byte double box and four 8-byte indices. Its layout is read directly from the source. The other sizes are measured with `sizeof` on the actual headers or included production distance implementation.

A proposed binary record with a double 3D box and four `size_t` fields is 80 bytes. This does not provide an application memory saving:

- The point node is 76 bytes. Preserve its proxy, group and extrema fields and the generic representation has 116 bytes of fields, rounded to 120 bytes by eight-byte alignment on x64. The probe includes this hypothetical record to verify its size. At the same node count, this is **44 extra bytes per node**, or 58% more node storage.
- The annotation mesh node is 72 bytes. Preserve its two source-index range fields and the generic record reaches 96 bytes. This is **24 extra bytes per node**, or 33% more node storage.
- The screen projection node is 64 bytes. Adding an unused third double box axis increases it to 80 bytes, or 25% more.
- The intersection node remains 80 bytes. A common struct removes no payload there.
- A 256-byte distance node contains up to four child boxes. Comparing it with one 80-byte binary node is invalid. A full four-way branch needs three binary branches. The proposed binary record does not retain the current four-child structure-of-arrays layout; a SIMD binary layout needs separate measurement. Tree shape, leaf occupancy and query distribution determine the result.

These are layout calculations, not process memory measurements. They exclude primitive storage and allocator overhead. A generic tree can use templates to retain each layout, but this then shares implementation code rather than storage. A source refactor should retain the same specialized records and benchmark contracts.

## Safe implementation boundary

Useful common code includes component-wise finite bounds checks, box union, outward interval expansion, checked IDs and immutable tree ownership. The utility must make float/double precision, closed/open overlap and 2D/3D dimensions explicit. Centralize cancellation checks only if each traversal keeps its current cancellation frequency.

Do not merge a replacement traversal until distance values, all intersection findings and their order, point marker identity/coverage, near-clipped annotation attachments and cancellation behavior agree with their current paths. Measure each query family separately. A faster nearest-distance traversal does not establish faster pair enumeration or screen picking.

The [PBRT BVH chapter](https://www.pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies) gives a primary-source cost model for ray trees. It supports the need to choose a layout for a query workload; it does not establish an optimal layout for Woby's nearest-distance and bounded-pair tasks.

Implementation status: audit complete. No replacement tree was added. Merge recommendation: accept a small utility refactor if needed for another change; reject a universal-tree performance project without a specified bottleneck and a measured candidate.
