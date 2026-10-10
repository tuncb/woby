# Data structure and algorithm simplifications

Woby can remove substantial work by separating geometry identity, geometry ownership, and display attributes. The strongest opportunities are immutable shared mesh storage, retaining the mapping from render vertices to source positions, and drawing analysis results from per-face records instead of expanded meshes. These changes would remove copies, synchronization rules, and conversion passes together.

This assessment applies to commit `974d8a583bfa977d2cfcba76f5ea9b419d8d3888`, inspected on 7 October 2026. It covers import, scene state and history, analysis, annotations, picking, rendering, and the relevant third-party interfaces. The proposals below were not implemented at the time of that inspection. Byte counts are representation calculations, not measured reductions in process memory or frame time.

| Priority | Change | Work it could remove | Confidence and scope |
| --- | --- | --- | --- |
| 1 | Share immutable geometry storage | Comparison input/result copies, annotation snapshots, mesh copies during undo preparation | High confidence; foundational change |
| 2 | Retain render-to-source position IDs | A double-precision position copy for every render vertex | High confidence; geometry access change |
| 3 | Store analysis values per face | Expanded quality vertices, distance submesh storage, comparison edge buffers | High confidence in storage savings; rendering validation required |
| 4 | Split analysis geometry from topology incidence and presentation | Full topology and overlays for intersections-only jobs; repeated graph construction | High confidence; staged algorithm change |
| 5 | Use one freeform evaluation path for fixed tessellation | CPU evaluation followed by equivalent GPU evaluation; duplicate CPU evaluation for normals | High confidence that work is duplicated; preserve visual fidelity |
| 6 | Give UV metrics and layout separate representations | Copies of all triangle metrics for presentation edits; mesh rebuilds on layout switches | High confidence; cache and display change |
| 7 | Use a two-dimensional UV broad phase | Quadratic scans of boxes disjoint on the other axis | Implemented incremental sweep with deterministic partial ordering and separate search limits |
| 8 | Retain the distance distribution | Repeated full scans for area above tolerance | High confidence; small independent change |
| 9 | Use CDT iterator input and reconsider global trim triangulation | Temporary batch vectors; potentially most interior-cell triangulation | Iterator input implemented; indexed boundary-cell prototype next; backward compatibility is out of scope |
| 10 | Use exact predicates without constructing intersections | Rational points, rational divisions, clipping output used only for a boolean | Promising; demanding numerical equivalence work |
| 11 | Separate persistent scene content from session state | Duplicate history representations and repeated save-format conversion | High confidence in duplication; wider scene change |
| 12 | Use typed graphics parameters | Uniform-name lookup and float encoding of integer parameters | Clear simplification; modest performance expectation |
| Research | Instance-aware spatial queries and direct OBJ corner access | World-mesh baking and tuple flattening | Potentially large; needs workload and semantic validation |

## Geometry ownership

Idea 1 is now implemented. See [shared geometry implementation and measurements](shared-geometry-performance.md) for the ownership rules, tests, and before/after results. The assessment below describes the original implementation.

`Mesh` combines owning vectors, source data, derived caches, and analysis-specific fields. It looks cheap to copy because several members are shared pointers, but its vertices, precise positions, indices, nodes, and batch IDs are ordinary vectors. This matters at several boundaries:

- `snapshotComparisonInputs` copies each participating `UiFileState`, including its entire mesh even if only one small part participates. This happens before launching the UV preparation worker.
- `comparisonWorldMesh` constructs transformed geometry. For non-UV analyses, the runtime currently performs that construction directly inside its update/scheduling path.
- `computeComparisonStages(comparisonSource)` copies the prepared mesh vectors again into `SurfaceComparison::source`.
- `buildAnnotationMeshCache` creates another mesh containing copied indices and 32-byte vertices with only their positions populated. It also creates a second cache wrapper to keep ownership acyclic.
- `prepareSceneHistoryStep` copies a live mesh into the prepared scene, even though history itself correctly excludes mesh buffers.

Sources: [Mesh storage](../src/model_mesh.h), [comparison snapshots and world assembly](../src/comparison_scene.cpp), [runtime scheduling](../src/comparison_runtime.cpp), [source result copying](../src/mesh_comparison.cpp), [annotation snapshots](../src/surface_annotation.cpp), [history preparation](../src/scene_history.cpp).

Introduce an immutable geometry owner, held by scene instances, worker inputs, and results. Keep editable scene properties in `UiState`. Build geometry mutably on a worker, validate it, and publish a `shared_ptr<const MeshGeometry>`. Operations that replace geometry publish a new owner and revision. Derived caches should own the geometry they need, without making the geometry own a cache that points back to itself.

This would make comparison snapshots proportional to participating instance metadata rather than file geometry. It would also let annotation preparation own its input instead of borrowing live spans and requiring cancellation/join before replacement. Revision and document checks must remain: ownership prevents dangling pointers, but cannot decide whether a completed result is still applicable.

For a mesh with `R` render vertices and `T` triangles, copying vertices, precise positions, and triangle indices costs approximately `56R + 12T` bytes, before nodes and other arrays. An annotation snapshot adds approximately `32R + 12T`. Sharing those arrays removes the copies; replacing the annotation snapshot's `Vertex` with a position-only structure would merely reduce one of them.

Do not make undo retain every removed geometry asset forever as an accidental consequence. Its current reload policy and memory ownership policy should remain explicit. Similarly, stage results can have immutable owners so exports share findings, extending the sharing already used for topology incidence.

**First experiment:** change ownership without changing vertex order, topology, or output. Measure snapshot latency, undo preparation latency, peak memory, and retained memory for one small selected part in a large file. Exercise removal/reload, vector relocation, undo/redo, and late worker completion.

## Source positions and render vertices

The [item 2 investigation](source-position-storage-investigation.md) covers this change after shared geometry storage was implemented. It includes a position-access probe, ownership requirements, and validation steps. Identity access has approximately the same cost in the probe; explicit mapping can make reads substantially slower and can retain unused source positions after file removal. Item 2 is now implemented; see the [implementation report](source-position-storage-implementation.md).

The [large-model application benchmarks](source-position-model-performance.md) measure actual comparison preparation, dimensions, and bounds on BearTrap and BusGameMap. They show a much smaller effect than the random-read probe: about 5-6% more comparison preparation time, with little change in most other measured operations.

The OBJ loader knows which source position produced every render vertex, but retains the coordinate instead of that relationship. At each new tuple, `buildObjMesh` copies a double position into `Mesh::precisePositions` and its float form into `Vertex::position`. The authoritative source position remains in `SourceMeshData::points`. UV and normal seams multiply the render vertices and therefore multiply the double positions too.

The importer path duplicates the arrays even when source and render indexing are identical: `captureSourceMesh` copies positions and triangle indices. Freeform append likewise writes positions to both precise and source arrays.

Sources: [OBJ tuple mapping](../src/obj_mesh.cpp), [position access and source capture](../src/model_mesh.cpp), [importer construction](../src/importer_host.cpp), [freeform construction](../src/freeform.cpp).

Retain a `renderVertexToSourcePoint` array of 32-bit IDs. Implement precise position lookup through that mapping. An explicit identity-mapping mode can avoid the mapping array for suitable importer and generated meshes. A transformed analysis mesh can have its own immutable position owner; this proposal does not force every kind of geometry into an imported-source representation.

Replacing a 24-byte copied coordinate with a four-byte ID saves **20 bytes per render vertex**, about **191 MiB for ten million render vertices**. An identity mapping can save the full 24 bytes. These savings exclude the source array, which remains available.

The important distinction is source identity versus geometric equality. Equal positions with different source IDs must remain distinct for original-index topology and duplicate reporting. Conversely, authored UV/normal seams remain distinct render vertices. Keep unused OBJ positions and importer `originalPointIds`; neither can be reconstructed reliably by welding the render mesh later.

This also gives point clouds a simpler identity chain. The current cloud retains marker ID, render-vertex mapping, float point data, and the mesh's position arrays. A direct source-position relationship can reduce that indirection without changing marker order or merging coincident points.

**First experiment:** implement the position lookup behind `meshPosition` and compare source IDs, seam behavior, unused points, large-coordinate origins, rebase behavior, mixed primitives, and freeform append. Measure random position access as well as memory; an extra lookup is a tradeoff.

## Analysis display records

The four item 3 display paths are now implemented: surface quality, UV quality, distance heatmaps, and comparison wireframes. The separate comparison-picking cleanup also removes the unused CPU sampled indices; picking continues to use the source mesh. See the [implementation and validation report](face-display-storage-implementation.md). The [initial investigation](face-display-storage-investigation.md) records the design choices and CPU locality trade-off. The assessment below describes the original code.

Several large arrays exist because `Vertex` is also used as a carrier for measurements:

- `surfaceQualityDisplayVertices` expands every triangle to three 32-byte vertices and distributes four face metrics across their UV slots. The shader reads those slots back by triangle corner number.
- `uvQualityVertices` also expands triangles to attach a face value and highlight state.
- `compareSurface` creates four subtriangles per source triangle, twelve vertices, and an identity index array. It retains four distances and four areas as well.
- Comparison source upload constructs six edge indices per triangle, even though the source renderer already draws triangle edges from its triangle buffer.

Sources: [surface quality display packing](../src/surface_mesh_quality.cpp), [UV display packing](../src/uv_quality.cpp), [distance expansion](../src/mesh_comparison.cpp), [comparison upload](../src/comparison_gpu.cpp), [shader reads](../shaders/native/woby.slang), [source edge preparation](../src/scene_mesh_preparation.cpp).

Keep the indexed geometry and add a per-face measurement buffer. A shader can fetch the source corner and the face record using a generated corner ID, or use primitive ID where supported. Flat normals can be generated from the three source corners or stored once per face. This approach fits the GPU pointer and shared-root model already provided by [NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI/blob/ae017a2f545abc0847e546cc7e84139bf3cc4241/README.md).

| Display payload per original triangle | Current representation | Candidate representation |
| --- | ---: | ---: |
| Four quality metrics | 96 bytes of expanded vertices | 16 bytes of metrics, plus optional normal/flags |
| Four distance samples | 384 bytes of expanded vertices | 16 bytes of GPU distances, plus optional normal/flags |
| Triangle wireframe | 24 bytes of edge indices | Derive endpoints from existing triangle indices |

These are incremental display payloads; the source geometry remains required. CPU statistics can retain double distances independently of the GPU float representation.

For distance display, the four subtriangles have fixed barycentric corners. The least disruptive experiment is to generate the same twelve corners in the vertex shader from the original three positions and four distance values. That removes stored expansion without changing the sample locations or triangle subdivision. A later barycentric fragment classifier could avoid generating subtriangles too, but requires separate coverage and MSAA validation.

The CPU identity index buffer alone costs **48 bytes per original triangle**. It is not uploaded by the distance path. The item 3 audit confirms that picking already uses the source mesh and dimensions consume those source pick parts; no production reader of sampled indices was found outside construction. Removing that array is a separate candidate. Update the sampled-result contract and tests, which currently assert the indices, together. Surface-quality CPU display vertices are already released after upload, so compact quality records reduce GPU storage and transient CPU preparation/upload memory.

Diagnostic overlays can similarly store source edge/face references and retrieve endpoints, rather than storing the same positions as a finding, bounds, line geometry, and fill geometry. Keep arbitrary constructed line segments representable; not every diagnostic is an original mesh edge.

**First experiment:** quality display first, followed by distance corner generation and comparison wireframes. Check screenshots, marker IDs, flat shading, depth, transparency, clipping, CPU queries, and both renderer backends. Measure GPU time as well as payload size: shader fetches and lost vertex reuse can offset some gains.

## Analysis geometry and topology stages

The [stage separation investigation](analysis-stage-separation-investigation.md) confirms the unused dependencies and measures the reuse opportunity on BusGameMap. It covers the benefits, memory trade-offs, correctness risks, and an incremental implementation order. The [implementation report](analysis-stage-separation-implementation.md) records geometry reuse, separate finding families, and worker display preparation.

Before the change, an intersections-only stage called `inspectSurfaceTopology`, built the complete graph and findings, and prepared topology overlays. It then discarded those extra products during publication. The new path builds or reuses immutable analysis geometry directly. Topology requests add incidence to the same geometry owner and compute only the requested finding families.

Sources: [stage execution and merge](../src/mesh_comparison.cpp), [topology construction](../src/mesh_topology.cpp), [intersection inputs](../src/mesh_intersections.cpp).

Construction now separates these products:

1. Selected source instances, validated positions, transformed vertex identities, collapsed-face filtering, and face references.
2. Edge/vertex incidence and connected components.
3. Detector findings such as links, boundaries, and fins.
4. Worker-prepared display buffers. GPU allocation remains eager for completed stages.

Intersections need the first product plus a bounding-volume hierarchy. Topology detectors add incidence and their own findings. An existing compatible topology result can provide the first product without rebuilding it. Cache keys must include geometry, membership, batching, topology mode, and relevant transforms; a hole threshold or a display toggle should not enter the geometry key.

This split also allows `DuplicateSource`-based detectors to run without first constructing a general-purpose render `Mesh` in world space. The code already carries source instances and transforms inside `DuplicateInput`; separate that useful contract from its current storage inside an analysis mesh. Prepare display geometry only when needed, on a worker.

There is still repeated construction: the edge-finding stage runs `inspectMesh`, which welds geometric vertices and sorts incidence, in addition to source topology. Surface quality builds its own exact-position edge adjacency too. Share compatible indexing and incidence products, but preserve their distinct semantics. `inspectMesh` exposes geometric-duplicate and numerical-degeneracy counts through the UI and control API. It cannot simply be deleted in favor of original-index topology, and quality's degeneracy threshold differs from exact collapsed-face filtering.

**Validation:** the implementation report covers cold and cached intersection requests, topology upgrades, finding identity and order, both topology modes, cancellation, bounded results, display publication, and retained storage. Its compatibility review keeps quality and legacy indexing separate because their identity and degeneracy rules differ.

## Freeform evaluation

**Implemented:** fixed freeform tessellation now uses the completed CPU mesh for rendering. `appendFreeformGeometry` reuses each `FreeformSample` for normal generation, while preserving authored normals and exceptional interior samples at singular boundaries. Native controls, parameter grids, trim triangulation, source IDs, and double source positions remain available.

The second GPU evaluation has been removed, together with its packing, temporary input buffers, compute shader, readiness flag, and float-safety selection. The ordinary upload supplies complete vertices and connectivity. The upload tests now require exact agreement with CPU positions, normals, UVs, and indices, including rebased rational and trimmed geometry.

The [initial investigation](freeform-evaluation-investigation.md) measured about 45% less time in an isolated evaluation loop. It also found 1.06 seconds of CPU packing work and 139.6 MiB of packed inputs for 1,000 keycaps. Those are baseline stage measurements. The [implementation report](freeform-evaluation-implementation.md) records application validation and before/after results.

GPU-first lazy or adaptive tessellation remains a separate architectural choice. Bounds, picking, annotations, and analysis require CPU geometry today. The fixed path does not add camera-adaptive refinement.

Sources: [CPU evaluation](../src/freeform.cpp), [ordinary upload](../src/scene_renderer.cpp), [exact upload checks](../tests/freeform_gpu_tests.cpp), [render and scene restoration checks](../tests/ctl_freeform_smoke.py).

## UV metrics and layout

Follow-up: [UV stage separation investigation](uv-stage-separation-investigation.md) measures the remaining costs after compact per-face display storage and recommends an incremental implementation order.

Implementation: [shared UV measurements, overlap results, and layout identity](uv-stage-separation-implementation.md). The physical layout mesh and current compact face format remain in use.

`UvQuality` mixes immutable triangle measurements and UV coordinates with settings, normalized values, overlap flags, distribution caches, and findings. A settings refresh copies the complete structure before modifying a few derived fields. The measured `UvTriangleQuality` size is **152 bytes** on this Windows x64 build; ten million triangle records alone occupy approximately **1.42 GiB**, before overlaps and findings.

`uvLayoutMesh` then represents UV display as another physical mesh, copying vertices/indices and constructing double positions from its float layout coordinates. Layout/view settings enter the preparation signature, although switching views does not change the underlying surface-to-UV measurements.

Sources: [UV types](../src/uv_quality.h), [normalization and statistics](../src/uv_quality.cpp), [layout construction and refresh](../src/comparison_scene.cpp).

Separate immutable raw metrics and overlap results from the current presentation selection. Derive normalized values from canonical values and per-patch normalization factors. Keep threshold findings as their own result, and encode highlighting/metric selection as display parameters or compact face data. The source geometry, measurements, overlap result, layout placement, and threshold selection can then have independent keys.

Represent layout through UV coordinates plus per-patch placement transforms. Rendering can select 3D positions or laid-out UV positions without building another `Mesh`. CPU picking and framing need an equivalent accessor. Missing/collapsed UVs, separated patches, triangle provenance, and switching back to the 3D surface must retain existing behavior.

The current code already caches distributions and has a quality-only refresh path. The opportunity is to remove its bulk copies and display repacking, not to add another general cache around it.

## UV overlap broad phase

Implemented in production with deterministic `uv_sweep_v1` ordering, two-dimensional candidates, and a separate bounded search counter. Reports distinguish candidate, pair, and search-work truncation. See the [implementation report](uv-overlap-sweep-implementation.md) for validation and the accepted partial-highlight change.

The [prototype investigation](uv-overlap-broad-phase-investigation.md) compares the current scan with eager and per-patch box trees. Both trees remove the disjoint-strip failure, but both are slower on BusGameMap. This led to an incremental sweep prototype with an interval index on the other axis. See the [raw box-tree results](uv-overlap-broad-phase-results.json).

The [incremental sweep follow-up](uv-overlap-sweep-investigation.md) now implements that prototype. It avoids the large tree-construction regression and completes the strip fixtures. On BusGameMap it takes about 2% more time within patches and 9.5% more across selected patches than the current bounded scan. Its deterministic pair order changes the subset of triangles highlighted when a limit stops the search. Production adoption must make that behavior explicit. See the [sweep results](uv-overlap-sweep-results.json).

`inspectUvOverlaps` sorts boxes on U and scans until U ranges separate, counting candidate work before testing V. Disjoint horizontal strips therefore trigger quadratic work even when there is no possible two-dimensional overlap.

A probe compiled the unchanged production overlap translation unit with its existing validity predicate and a no-op statistics sink. It used 2,001 valid triangles in one patch: triangle `i` has UVs `(0,2i)`, `(1,2i)`, `(0,2i+1)`. Statistics were excluded; overlap enumeration and limits were unchanged.

| Same disjoint geometry | Candidate count | Findings | Truncated |
| --- | ---: | ---: | --- |
| Horizontal rows | 2,000,000 | 0 | Yes |
| U and V exchanged | 0 | 0 | No |

This demonstrates an orientation-sensitive completeness limit, not a false-positive intersection. The unlimited horizontal scan would visit `2001*2000/2 = 2,001,000` pairs.

Use a two-dimensional box hierarchy or a sweep with an interval structure on the other axis. Choosing the sweep axis by estimated interval overlap is a smaller first step, but cannot remove the general worst case. Preserve bounded work and partial-result reporting: dense real overlap can still require quadratic output. Define stable candidate ordering if the first retained pairs under a limit must remain reproducible.

Source: [UV overlap implementation](../src/uv_overlap.cpp). The exact narrow-phase test should remain separate from this change.

## Distance distributions

`compareSurface` already sorts sample IDs to compute the area-weighted 95th percentile, then discards the order. `surfacePercentAboveTolerance` scans every distance and area on each query, including calls from the comparison UI.

Retain sorted distances with cumulative areas. Compute both percentile and area-above-tolerance from this one distribution; an `upper_bound` preserves the existing strict `distance > tolerance` rule. This changes repeated queries from a full sample scan to a logarithmic lookup. Surface quality already uses the same pattern through `sortedSizes` and `prefixAreas`.

Sources: [distance statistics and tolerance query](../src/mesh_comparison.cpp), [UI consumer](../src/comparison_view.cpp), [existing prefix-area pattern](../src/surface_mesh_quality.cpp).

The distribution consumes retained memory, so compare it with simply caching the last tolerance query. The latter is a smaller change when only one threshold is queried repeatedly. A distribution is more useful for live threshold changes and further distribution queries.

The four midpoint subtriangles also have equal area mathematically. One parent-face area could replace four stored areas. However, current areas are computed from rounded midpoint coordinates; preserving numerical behavior requires extreme-scale and cancellation tests. Do not assume bitwise equivalence from the mathematical identity.

## CDT and trim representation

The [joint investigation of 9a and 9b](trim-input-investigation.md) found exact CDT output equality for iterator input across 15 synthetic cases, with no consistent timing improvement. 9a is now implemented with the same batches and cancellation points. At a 256-cell grid resolution, only 1.6% of cropped diagonal-strip cells and 3.1% of annular cells touch trim boundaries. Those counts show potential work reduction, not a measured replacement speedup. A naive boundary scan can exceed the cost of global CDT. Use a spatial index in the next prototype. The user has excluded backward compatibility, including old annotation attachments, from 9b. See the [raw results](trim-input-results.json).

Woby constructs temporary vertex vectors for every 1,024-element insertion batch and edge vectors for every 256-element batch. The installed CDT 1.4.4 headers already provide iterator-range overloads with coordinate/endpoint getters. Use those overloads with the existing batch boundaries to remove the copies while retaining cancellation opportunities. The [CDT API source](https://artem-ogre.github.io/CDT/Triangulation_8h_source.html) documents this interface; the installed headers were also checked because online documentation now describes a newer release.

Do not casually replace all batches with one call: `Auto` uses a different insertion strategy for the initial batch and subsequent calls. This is documented by [CDT](https://artem-ogre.github.io/CDT/index.html) and present in the installed source. Bulk insertion needs cancellation support and triangulation-order validation, including saved annotation fingerprints.

The deeper opportunity is to represent trimming as a grid domain with exceptional boundary cells. Current code constrains a rectangular grid and trim boundaries globally, asks CDT to resolve their intersections, and filters output by region containment. Emit ordinary retained interior cells directly; clip/triangulate only cells crossed by trim boundaries. Shared grid-edge intersection IDs would keep neighboring cells connected.

This could remove much of the global triangulator input, triangulation, and remapping. It is a research direction because knot-line constraints, rational boundary sampling, holes, multiple regions, boundary contacts, deterministic IDs, and cracks all need to survive. Start with long diagonal or annular trims whose outer bounding rectangle still contains much discarded area. The existing outer-bounds crop is already implemented and should not be counted as a new opportunity.

Also avoid blindly substituting `eraseOuterTrianglesAndHoles` for the current filter: Woby marks internal grid edges as constraints too. CDT's boundary-layer interpretation cannot distinguish those grid edges from trim boundaries without additional representation changes.

Sources: [trim construction](../src/freeform_trim.cpp), [previous crop measurements](freeform-loading-performance.md).

## Exact intersection predicates

The self-intersection detector asks a boolean question, but its exact fallback converts both triangles to `cpp_rational`, clips segments, constructs rational intersection points, and checks whether those points lie on an allowed shared feature. The report stores the original triangle coordinates, not the constructed intersection points.

A predicate-only triangle-overlap algorithm could avoid constructing those exact coordinates. Robust orientation signs, projected coplanar tests, and explicit shared-feature classification are sufficient in principle. The repository already obtains adaptive `orient2d` and `orient3d` implementations through CDT's installed `predicates.h`; [Shewchuk's predicate work](https://www.cs.cmu.edu/~quake/robust.html) explains the adaptive approach.

Sources: [rational clipping and shared features](../src/mesh_intersections.cpp), [UV rational orientation fallback](../src/uv_overlap.cpp), [existing exact-oracle tests](../tests/mesh_intersections_tests.cpp).

Keep the rational implementation as an independent oracle during development. A generic closed-triangle intersection test is insufficient: allowed shared vertices/edges and disallowed coplanar overlap are part of Woby's contract. Extreme exponents, subnormals, overflow, contraction, and floating-point compiler settings also need explicit coverage; a dependency advertising robust predicates does not prove equivalence over every input Woby currently accepts. Retain a guarded rational fallback for unsupported ranges if necessary.

This change can remove a demanding algorithm and temporary representations. Its speed benefit depends on how often real inputs enter the existing exact fallback; first count that frequency on representative intersection workloads.

## Scene content and history

Persistent properties are repeated in `UiGroupState`, `SceneGroupSettings`, and their mapping functions, with similar duplication for files and folders. `SceneSnapshot` stores both a metadata-only `UiState` and a `SceneDocument`, plus identities. Annotation geometry therefore appears in both snapshot representations. A single annotation segment is 48 bytes here; one million segments imply about **91.6 MiB** for the two segment arrays in one snapshot, before live geometry and other overhead.

Sources: [logical state](../src/ui_state.h), [save records](../src/scene_file.h), [conversion](../src/ui_state.cpp), [history representation](../src/scene_history.h), [snapshot construction](../src/scene_history.cpp).

Extract persistent settings structs that `UiState` owns alongside derived/session fields. Use one typed logical-content snapshot for history and dirty equality; perform the positional file/group mapping only at the persistence boundary. Give large immutable annotation geometry shared ownership so a color edit does not copy every segment. Preserve content equality for drags that return to their starting value; a revision or hash alone is not an equivalent replacement.

A stable ID-to-object index and parent/child ID representation are a further option. They could simplify recursive folder lookup, file-index repair after removal, and repeated ancestor storage. A smaller first step is a revisioned lookup table over the existing tree. The recent scene query caches already eliminate many steady-state traversals, so a full scene arena rewrite needs evidence from structural edits or very deep hierarchies.

Avoid switching to a generic ECS or event-sourced undo merely to unify types. Neither follows from the duplication found here, and both would expand the migration substantially.

## Graphics parameter representation

The renderer already owns a typed `WobyRoot`, but callers pass uniform handles. `setUniform` resolves a resource, compares its string name, chooses a root field, and copies vec4 data into it. Marker IDs and offsets are also split into float halves and recombined by shaders.

Replace name-based uniform writes with typed parameter structs or typed field setters. Carry integer IDs and offsets as integers in the shared C++/Slang root layout. Keep resource lifetime management, frame ordering, staging, and fences in the runtime adapter.

Sources: [uniform dispatch](../src/graphics.cpp), [graphics interface](../src/graphics.h), [shader root](../shaders/native/root.h), [ID reconstruction](../shaders/native/woby.slang).

This removes compatibility encoding and several invalid combinations. It does not justify bypassing the adapter's resource ownership or synchronization, and it is unlikely to rival mesh-copy removal in memory savings.

## Larger alternatives and limits

**Query in local coordinates.** Scene transforms use translation, rotation, and a scalar scale. For one similarity-transformed part, transform a distance query into its local frame and scale the distance back. A local triangle hierarchy can then survive placement changes. A second hierarchy over instances can select candidate parts, avoiding repeated baking of large world meshes. Independently transformed parts, cross-part intersections/welding, floating-point rounding, and exact-position equality make this a conditional fast path. A blanket claim that all detectors are transform-invariant would be wrong.

**Keep OBJ corner indexing.** A shader can read position, normal, and UV indices separately, avoiding the global tuple-to-render-vertex mapping and seam-expanded buffer. The unified parser already produces those tuples. However, vertex reuse, generated normals, marker identity/order, and a larger per-corner index payload can erase the benefit. Prototype it for attribute-heavy meshes; preserve the inexpensive position-only path. The repository's [mapping measurements](vertex-mapping-detail-2026-09-28.md) already show why changing only the secondary hash is unlikely to solve most loading cost.

**Project annotations on the GPU.** Stored barycentric anchors and source triangle IDs could drive line expansion directly, removing the camera-dependent projected-vertex cache and repeated CPU upload. The current projection extends strokes in the source triangle's depth plane to prevent uphill edges sinking into the surface. A generic line shader would lose that behavior; preserve it along with near clipping, bridge segments, and export pixel scale. This is most relevant when orbiting around many annotation segments.

**Use compact topology IDs selectively.** Measured x64 sizes are 88 bytes for `TopologyVertex`, 80 for `TopologyFace`, and 40 for `TopologyEdge`, before incidence arrays. Four spans account for 64 bytes of a topology vertex. Offset/count slices into an immutable owner can replace spans, and checked 32-bit topology IDs can reduce many 64-bit arrays. Do not globally narrow `size_t`: selected source instances can expand cardinality, and three edge uses per face can overflow before a vertex count does. This is a memory optimization after stage decomposition, which can avoid allocating whole structures at all.

**Share spatial infrastructure selectively.** Distance, intersection, point, and annotation hierarchies have different layouts and semantics: doubles versus floats, nearest queries versus pair enumeration, source order versus Morton order, and proxy LOD. Shared box utilities, ownership, and traversal primitives are plausible. Forcing them into one universal tree can remove useful specialization and add conversions.

**Avoid a wholesale library replacement.** Ordinary manifold half-edge structures are a poor default for a tool whose job includes non-manifold input, duplicates, and winding contradictions. A broad geometry-library migration should first prove those contracts on the existing corpus. Likewise, existing [hash-table experiments](hash-table-performance.md) do not establish that replacing the specialized insertion-ordered analysis index removes any transformation. The OBJ parser already writes caller-owned double positions and moves them into source storage; proposing to eliminate that already-removed copy would be obsolete.

## Implementation order and validation

Start with small independent deletions: reuse the CPU freeform sample for normals, use CDT iterator ranges, and cache or retain the distance distribution. Next publish immutable geometry owners and retain source-position mappings. Those changes make the analysis-stage split and compact display records much easier. Treat predicate-only intersections, cell-based trimming, local-coordinate queries, and multi-index OBJ rendering as separate experiments.

For geometry changes, compare source IDs, triangle order, topology modes, finding order/counts, precise positions, signed zero, seams, unused points, per-source/per-volume boundaries, coordinate origins, and annotation persistence. For ownership changes, exercise cancellation, removal, undo/redo, reload, and late results. For rendering changes, compare depth/coverage, marker IDs, transparency, screenshots, CPU queries, and both supported backends.

Measure import and analysis-ready latency, longest UI stall, peak and retained CPU/GPU memory, and repeated-query cost separately. Require equivalent complete results; early truncation, omitted overlays, or changed visibility is not a speedup. Use the same release configuration and input hashes, alternate before/after runs, and avoid concurrent builds or tests for timings.

The current Debug build completed without warnings using `vs2026-vcpkg` with headless tests enabled. All **1,010 CTest entries passed**, including 12 graphics tests and four slow tests, in 355.65 seconds. The standalone overlap/size probe compiled with MSVC `/W4 /WX`; it records deterministic representation sizes and candidate counts, not performance timings. Production code remains unchanged.

Reproduce the baseline from the repository root after making the graphics toolchain available:

```powershell
cmake --preset vs2026-vcpkg -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 2
```
