# Geometry library replacement investigation

Decision: keep the current source geometry and incidence model. Do not approve a broad migration to a manifold half-edge mesh as an optimization. A narrow library algorithm can still be useful if it accepts the required input and retains source identities.

This audit applies to commit `46a27585c5bc8638a8bde5d601e5cead71376631` on 11 October 2026. This report uses working item number 18 for the unnumbered library replacement entry.

## Why an ordinary half-edge mesh changes the task

Woby inspects defective input. It must keep coincident points with different source IDs, duplicate faces, more than two faces on an edge, winding conflicts, disconnected vertex links and cross-part source references. Its graph stores a variable-length list of uses per edge. An ordinary paired half-edge record gives an edge only two sides.

The [topology tests](../tests/mesh_topology_tests.cpp), [intersection oracle tests](../tests/mesh_intersections_tests.cpp) and [duplicate tests](../tests/mesh_duplicates_tests.cpp) already define these contracts. Original-index and exact-position topology are separate operations. Repairing a soup to fit a manifold data structure can change either result before the detector starts.

The current [CGAL polygon mesh processing manual](https://doc.cgal.org/latest/Polygon_mesh_processing/index.html) defines its polygon mesh as a 2-manifold, with two faces per edge including the null boundary face. The [CGAL 5.3 repair documentation](https://doc.cgal.org/5.3/Polygon_mesh_processing/index.html) describes operations that duplicate non-manifold vertices and merge duplicate points or polygons. This is useful for repair tasks, but those changes are visible evidence in Woby's inspection task. This is a limitation of a particular representation, not a claim that all CGAL algorithms require a manifold mesh.

## Evidence and cost boundary

The [compact topology probe](../experiments/simplification-16/main.cpp) adds these counts to each real-model sample: maximum edge incidence, edges with more than two uses, winding conflicts, orientation contradictions, vertices with several source references and duplicate vertex triples. It records both topology modes. These counts show which inputs need extra side records or repair before a two-sided edge adapter can operate. The raw values are in [item 16 results](simplification-16-results.json).

| Whole-source input and mode | Faces | Edges with more than two uses | Maximum uses on one edge | Winding conflicts | Duplicate canonical vertex triples |
| --- | ---: | ---: | ---: | ---: | ---: |
| BusGameMap, original index | 1,054,306 | 0 | 2 | 0 | 0 |
| BusGameMap, exact position | 1,054,306 | 116 | 4 | 24 | 2 |
| Powerplant, original index | 12,758,950 | 1,079,616 | 120 | 192 | 58,099 |
| Powerplant, exact position | 12,758,950 | 1,981,753 | 122 | 192 | 451,049 |

The selected triangle parts form one identity-transform source batch. Per-volume batching can isolate contacts and produce different counts. These are valid whole-source workloads, rather than an assertion that the default scene always reports these totals. Each count is identical across all samples and graph representations. Exact-position Powerplant also has 557,516 vertices with multiple source references; a library adapter must retain those references after welding.

An adapter must preserve a map from library vertices/faces back to every original source reference. It must also preserve excluded collapsed faces and unavailable sources. A library's count of repaired manifold faces is not an equivalent detector result. Splitting one non-manifold vertex into several copies needs a many-to-one mapping; dropping duplicate faces needs a retained duplicate record.

For illustration, a small 32-bit half-edge record with five IDs costs 20 bytes. Three half-edges per triangle cost **60 bytes per face**, before vertices, face records, provenance and exceptional edge-use lists. This is not a measured CGAL object size. Keeping it beside the current graph adds this payload. Replacing the graph can reduce some incidence arrays, but then all detector readers and non-manifold side records need a new implementation. Item 16 measures a much smaller change that retains the existing incidence algorithm.

There is no implemented library adapter here, so there is no measured speedup, process memory reduction or library build-time result. A broad migration has no selected library, algorithm bottleneck or validated representation. These missing inputs are sufficient to reject that project as a performance merge proposal. Installing a large library and timing a repaired mesh would not answer the required semantic question.

## Conditions for a narrow trial

Choose one operation, such as a predicate or a nearest-distance tree, with an independent oracle. Retain immutable input geometry and report source IDs outside the library. Measure conversion, algorithm work, output conversion and peak retained memory separately. Keep the complete comparison input, including defective fixtures, in the test corpus.

Require equal complete results before comparing time. Preserve all requested finding families, coordinates, source batching, budget behavior and cancellation. A library that repairs input must expose repair as a separate user operation before it can be considered for this route.

Implementation status: audit complete. Merge recommendation: reject the wholesale migration. Evaluate narrow algorithms only when they remove a measured cost and pass these contracts. [Item 10](simplification-10-predicates-investigation.md) and [item 13](simplification-13-instances-investigation.md) supply such bounded trials.
