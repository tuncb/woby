# Item 16: compact topology slices and IDs

This investigation uses production commit `46a27585c5bc8638a8bde5d601e5cead71376631`. It adds isolated probes. It does not change topology construction or findings in the application.

## Decision scope

Implement a compact graph path when lower analysis memory is the objective. Keep a checked preflight and a wide fallback, and keep source identities and analysis geometry wide. The evidence supports about 50% less retained and peak graph allocation, or about 38% less geometry-plus-graph array payload, on both tested large models.

Treat this as a memory/performance tradeoff. After removing proved redundant narrowing checks, Powerplant construction remains about 13% to 16% slower. Current-executable traversal is faster, but the traversal ranking changed across builds and does not supply a stable storage-only speed claim. A production merge needs the full detector/export/ownership tests below and an application analysis measurement that accepts the construction cost. The experimental graph is suitable as an implementation reference; it is not a production patch ready to merge.

## Current representation

The original proposal predates [analysis stage separation](analysis-stage-separation-implementation.md). Current topology records are smaller than the original 88-byte vertex and 80-byte face:

| Record on x64 | Current bytes | Content |
| --- | ---: | --- |
| `AnalysisVertex` | 24 | Double position |
| `AnalysisFace` | 48 | Source reference and three wide vertex IDs |
| `TopologyVertex` | 64 | Four spans |
| `TopologyFace` | 32 | Three edge IDs and component ID |
| `TopologyEdge` | 40 | Two vertex IDs, incident-use span, two flags |
| `TopologyEdgeUse` | 16 | Face ID and orientation flag |

[`SourceAnalysisGeometry`](../src/mesh_topology.h) owns positions, faces, point provenance, and reference offsets. [`TopologyIncidence`](../src/mesh_topology.h) already owns flattened vertex-face, vertex-edge, boundary-edge, and edge-use arrays. Spans point into these immutable owners. The graph is shared across result and export copies. The opportunity is to compress a graph that is needed; intersections-only work already avoids incidence allocation.

## Prototype contract

The [source](../experiments/simplification-16/main.cpp) implements the current counting-sort edge construction, canonical edge ordering, component traversal, and orientation-constraint traversal with two alternative storage formats:

| Record | Checked 32-bit slices; wide IDs | Checked 32-bit slices and IDs |
| --- | ---: | ---: |
| Vertex | 32 bytes | 32 bytes |
| Face | 32 bytes | 16 bytes |
| Edge | 32 bytes | 20 bytes |
| Edge use | 16 bytes | 8 bytes |
| Incidence element | 8 bytes | 4 bytes |

An offset/count slice contains two `uint32_t` values. Access reconstructs a temporary span from the immutable array owner. The source geometry, positions, source face references, point references, and reference-offset array stay unchanged. Components keep the current vector-of-vectors shape; the compact variant narrows their IDs. Findings stay outside this probe. Thus the figures do not count savings from narrowing source geometry or flattening components.

Point-reference slices are also redundant with `SourceAnalysisGeometry::referenceOffsets`. A later variant could derive them from the existing offsets and reduce a compact vertex from 32 to 24 bytes. That would save another eight bytes per topology vertex, with two offset reads during reference access. This is a representation calculation, not a measured result of this prototype.

Each run compares every reference, incidence element, edge endpoint, edge-use orientation, face edge, component ID, component order, and winding/contradiction flag against production. It also checks a traversal checksum. The self-test covers original-index and exact-position modes, coincident positions with different source IDs, duplicates, a collapsed face, a non-manifold edge, disconnected components, transformed repeated source input, unavailable sources, copy/relocation, cancellation, and overflow rejection. A five-quad Möbius strip requires nonzero orientation-contradiction flags in both modes and compares those flags against production.

The build timings exclude source import and analysis-geometry construction. Traversal timings scan vertex incidence, face incidence, edge endpoints, flags, and edge uses ten times. They do not measure complete topology detectors, export, application scheduling, or GPU work. Equal graph records are strong evidence for the storage change; production adoption still needs detector and export regression tests.

Each OBJ measurement selects all triangle parts from that file into one source batch with identity transforms. This measures source-wide incidence in each topology mode. The default per-volume analysis can split the file into more isolated batches; its graph counts and boundaries can differ. The probe uses the same selection for production and both candidates.

## Initial measured results

[Original raw measurements](simplification-16-checked-results.json) retain the first trial. Its [preserved source](../experiments/simplification-16/checked-main.cpp) has SHA-256 `fe2621b40a9eb0173dc1933209cca137504227f950f7ba002bfd6f81fedc50f6`, matching that executable's recorded source hash. The first three inputs have five Release samples per mode. Powerplant has three. All 36 samples verify every graph record and order for both candidates. All timed traversal checksums match production, and traversal allocates zero bytes.

The host is an AMD Ryzen 7 5800H, with eight cores and 16 logical processors. Each script ran in an exclusive measurement window. Raw provenance records the base commit, CPU, UTC completion time, input hashes, executable hash, probe and allocation-wrapper hashes, and unchanged production-source hashes. The geometry stage and model import are outside the timings below.

| Input and mode | Vertices | Faces | Edges | Components |
| --- | ---: | ---: | ---: | ---: |
| BusGameMap; original index | 535,799 | 1,054,306 | 1,589,988 | 156 |
| BusGameMap; exact position | 535,369 | 1,054,306 | 1,589,594 | 140 |
| Powerplant; original index | 5,984,052 | 12,758,950 | 18,112,504 | 175,887 |
| Powerplant; exact position | 5,424,561 | 12,758,950 | 17,162,306 | 121,001 |
| `topology.obj`; both modes | 19 | 10 | 23 | 5 |
| `attached-fin.obj`; both modes | 5 | 5 | 8 | 1 |

The graph array-capacity payload has a stable reduction on both large models. Values below are MiB (`1 MiB = 1,048,576 bytes`). Analysis geometry is shared and remains wide.

| Input and mode | Graph arrays: production / slices / compact MiB | Unchanged geometry MiB | Compact graph saving | Geometry plus graph saving |
| --- | ---: | ---: | ---: | ---: |
| BusGameMap; original index | 231.047 / 202.565 / 115.526 | 72.802 | 50.00% | 38.02% |
| BusGameMap; exact position | 231.020 / 202.555 / 115.512 | 72.799 | 50.00% | 38.02% |
| Powerplant; original index | 2720.393 / 2399.587 / 1362.570 | 858.001 | 49.91% | 37.95% |
| Powerplant; exact position | 2637.104 / 2340.622 / 1320.134 | 853.732 | 49.94% | 37.73% |

Slices alone save 11.2% to 12.3% of these large graph payloads. Adding narrow IDs saves about half. The small topology sample changes from 4,040 to 2,092 payload bytes; the fin sample changes from 1,392 to 708 bytes. These are useful contract fixtures, but their timing differences are too small for an application-performance decision.

Requested allocation memory includes graph owners and allocator requests beyond the numeric array payload. It excludes the source mesh and the already resident analysis geometry. Peak values include direct-build scratch.

| Input and mode | Production retained / peak MiB | Slices retained / peak MiB | Compact retained / peak MiB |
| --- | ---: | ---: | ---: |
| BusGameMap; original index | 231.050 / 262.538 | 202.568 / 234.056 | 115.529 / 131.269 |
| BusGameMap; exact position | 231.023 / 262.484 | 202.557 / 234.018 | 115.514 / 131.242 |
| Powerplant; original index | 2720.489 / 3088.312 | 2399.682 / 2767.506 | 1362.649 / 1544.156 |
| Powerplant; exact position | 2637.340 / 3000.842 | 2340.858 / 2704.360 | 1320.270 / 1500.421 |

The initial candidate does not show a speed gain. Times below are sample medians in milliseconds. Each scan entry is ten complete graph traversals.

| Input and mode | Build: production / slices / compact ms | Ten scans: production / slices / compact ms |
| --- | ---: | ---: |
| BusGameMap; original index | 402.231 / 443.340 / 426.903 | 138.290 / 147.052 / 145.666 |
| BusGameMap; exact position | 491.789 / 540.727 / 499.354 | 136.926 / 146.940 / 145.489 |
| Powerplant; original index | 4794.455 / 5799.493 / 5454.587 | 1565.728 / 1675.262 / 1661.135 |
| Powerplant; exact position | 4751.281 / 5641.545 / 5633.071 | 1509.931 / 1633.089 / 1596.418 |

For this checked implementation, compact construction is 1.5% to 6.1% slower on BusGameMap and 13.8% to 18.6% slower on Powerplant. Compact scans are 5.3% to 6.3% slower on these models. Powerplant original-index production build ranges from 4,740 to 5,145 ms; compact ranges from 5,451 to 6,024 ms. Exact-position production ranges from 4,734 to 4,954 ms; compact ranges from 5,593 to 5,724 ms. The large-model build regression is present across these samples.

These results describe the candidate implementation. They do not prove that narrow storage must be slower. Construction has additional checked narrowing at slice and corner writes. The candidate scan reconstructs pointer/count spans from owner data and offsets, while production stores pointer/count spans. Address reconstruction, zero extension, record stride, and compiler code generation are possible scan costs. The Release library does not enable span hardening or iterator debug checks, so the source does not support a claim that span bounds checks cause the scan regression.

## Controlled construction follow-up

The fourth backend, `compact_proved`, uses exactly the same `Graph<uint32_t>`, scratch arrays, reserve/release stages, sort keys, loops, component traversal, orientation traversal, cancellation sites, and scan function as the checked compact backend. It changes only hot-loop narrowing covered by the invariants below. It retains all cardinality preflights and the constant-time boundary-count check.

| Proof | Narrowing that follows |
| --- | --- |
| Trusted immutable `buildAnalysisGeometry` output has valid vertex indices and monotone reference offsets ending at `R`; initial preflight gives `V`, `R`, and `3 F` at most `UINT32_MAX` | Reference slices, face-degree slices, other endpoint, and corner IDs fit |
| Actual edge-count preflight gives `2 E <= UINT32_MAX` | Edge IDs, vertex-edge degree slices, and prefix offsets fit |
| Each incident-use run lies inside the `3 F` corner array | Edge-use slice offsets and counts fit |
| Boundary incidences are a subset of the `2 E` vertex-edge incidences | Boundary degree slices and prefix offsets fit |

Arithmetic stays wide until the final cast. The contract tests compare full graph records, order, flags, scan results, relocation, cancellation, and exact checked/proved array payload. The separate [follow-up runner](../experiments/simplification-16/run-proved.ps1) uses three samples per mode. It selects production and proved compact on BusGameMap, and all four backends on Powerplant. The latter compares checked and proved construction in the same executable and campaign; the old and new binaries alone cannot establish that removing checks caused a timing change. The runner also checks each real-model capacity and count against the preserved checked trial.

Both revised Debug and Release contract tests passed after warning-free builds. [Follow-up raw measurements](simplification-16-proved-results.json) record executable SHA-256 `569dcf02726388d1eab59436b70a2baae44971518b2995d05a6396eb3cf128da` and source SHA-256 `da4a6f98f8289c486b1e8f97c3392d1d3f0feff77ef953ec60d96a7f2736a434`. All twelve samples pass full-record verification and the preserved graph count/capacity checks. Requested retained/peak memory and numeric payload are identical for checked and proved compact graphs.

| Input and mode | Production build ms | Checked compact build ms | Proved compact build ms | Proved versus checked | Proved versus production |
| --- | ---: | ---: | ---: | ---: | ---: |
| BusGameMap; original index; two backends | 414.546 | Not timed | 414.345 | Not compared | 0.05% faster |
| BusGameMap; exact position; two backends | 495.657 | Not timed | 491.120 | Not compared | 0.92% faster |
| Powerplant; original index; four backends | 4640.373 | 5462.114 | 5304.033 | 2.89% faster | 14.30% slower |
| Powerplant; exact position; four backends | 4765.887 | 5684.184 | 5509.863 | 3.07% faster | 15.61% slower |

Removing proved hot-loop checks improves Powerplant construction, but does not remove its build regression. In original-index mode, all three proved samples lie between 5,281 and 5,309 ms; checked samples lie between 5,451 and 5,498 ms. Exact-position proved samples lie between 5,496 and 5,580 ms; checked samples lie between 5,683 and 5,759 ms. BusGameMap production and proved build ranges overlap, so the small median differences there do not establish a speed improvement.

The scan contains no narrowing checks. Checked and proved use the same scan template instantiation. Its ranking reverses between campaigns even though the scan source did not change:

| Campaign and mode | Production ten scans ms | Checked compact ten scans ms | Proved compact ten scans ms |
| --- | ---: | ---: | ---: |
| Initial Powerplant; original index; three backends | 1565.728 | 1661.135 | Not present |
| Initial Powerplant; exact position; three backends | 1509.931 | 1596.418 | Not present |
| Follow-up Powerplant; original index; four backends | 2140.867 | 1734.263 | 1718.674 |
| Follow-up Powerplant; exact position; four backends | 2139.025 | 1664.770 | 1693.636 |

The follow-up executable also gives BusGameMap production/proved medians of 186.251/151.868 ms and 195.365/156.015 ms for the two modes. Do not attribute this reversal to removed construction checks. The executable, its call paths, and the number of resident graphs differ from the initial trial. The bounded [same-executable envelope runner](../experiments/simplification-16/run-envelope.ps1) repeats production/proved alone on Powerplant without a source or executable change. It records start/end timestamps and asserts unchanged counts and capacities against the all-four run. Its final results below bound the envelope effect while leaving the cause of the old/new reversal unresolved.

A read-only disassembly audit of the current Release object confirms that each timed scan lambda inlines its scan body, and the wide scan still executes ten passes. Checked and proved compact use one `Graph<uint32_t>` scan-lambda specialization. There is no source or current-object evidence that removed construction checks can cause their scan differences. The old object is not retained, so a compiler or code-layout explanation for the old/new reversal remains a hypothesis.

All graphs remain alive until the end of each sample. Powerplant original-index graph payload resident during scans is 6.331 GiB in the original three-backend trial, 7.661 GiB in the new four-backend trial, and 3.987 GiB in the two-backend envelope. These sums exclude source mesh, analysis geometry, and allocator overhead. Verification scans production before counting flags and references, constructing a large face-identity set, and sorting face vertex triples. It is not an immediate common warm-up for each timed backend. The order alternates across three samples, which weights one order twice and the reverse order once. A future traversal-only study could use an immediate per-backend warm-up and a separate stable scan translation unit; no such change is part of these measurements.

The final [envelope measurements](simplification-16-envelope-results.json) ran from `2026-10-10T23:47:08Z` to `23:49:49Z`. Source and executable hashes match the all-four trial exactly. All six samples pass full-record verification; the model hash, graph counts, array capacities, and requested retained/peak graph memory are unchanged.

| Powerplant mode; two backends | Production build ms | Proved build ms | Proved build change | Production ten scans ms | Proved ten scans ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original index | 4658.783 | 5281.793 | 13.37% slower | 2230.830 | 1722.472 |
| Exact position | 4834.947 | 5572.835 | 15.26% slower | 2165.276 | 1675.569 |

The current-executable scan ranking holds for every envelope sample and both scan orders. Original-index production scans range from 2,213 to 2,255 ms, compared with proved scans from 1,719 to 1,784 ms. Exact-position ranges are 2,141 to 2,201 ms and 1,675 to 1,705 ms. Median proved scans are 22.8% and 22.6% faster in this envelope. Reducing resident graph payload from four graphs to two does not restore the old ranking: production scans are still 4.2% and 1.2% slower than the same-executable all-four medians. Extra fourth-graph residency alone is therefore insufficient to explain the old/new reversal.

The decision evidence that survives all campaigns is unchanged graph semantics, about half the graph allocation, and a persistent large-model construction cost. The isolated traversal result is tied to the compiled probe and its envelope. Do not translate it into a complete-detector or application-speed claim. No further timing study is required to decide whether to implement the memory path; a production merge still requires the integration measurement.

## Source-graph contract evidence for item 18

These counts describe accepted input defects that a replacement topology library must preserve or explicitly map. They use the same one-batch selection as the memory trial.

| Input and mode | Edges with more than two uses | Maximum uses on one edge | Winding conflicts | Orientation contradictions | Duplicate source face identities | Duplicate vertex triples |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| BusGameMap; original index | 0 | 2 | 0 | 0 | 0 | 0 |
| BusGameMap; exact position | 116 | 4 | 24 | 0 | 0 | 2 |
| Powerplant; original index | 1,079,616 | 120 | 192 | 0 | 0 | 58,099 |
| Powerplant; exact position | 1,981,753 | 122 | 192 | 0 | 0 | 451,049 |
| `topology.obj`; both modes | 1 | 3 | 1 | 0 | 0 | 1 |
| `attached-fin.obj`; both modes | 1 | 3 | 0 | 0 | 0 | 0 |

A duplicate source identity means the same `(fileId, partId, triangleId)` is present more than once. A duplicate vertex triple means a later face has the same sorted three graph vertex IDs, regardless of winding. These are distinct contracts. Exact-position mode produces 399 BusGameMap vertices and 557,516 Powerplant vertices with more than one point reference. Its net vertex-count reductions are 430 and 559,491, respectively. Source references must survive those merges.

There are no orientation contradictions in these real-model runs. This does not establish that the input domain is orientable: the Möbius contract fixture has contradictions. Also, the current orientation traversal crosses two-use edges only. A non-manifold edge can split that constraint traversal. Repairing these accepted source graphs into a manifold half-edge mesh can change connectivity, duplicates, provenance, and defect findings.

## Cardinality rules

Do not narrow `size_t` throughout the code. Source and scene identities remain 64-bit. Source face/point references remain wide. A topology graph can contain multiple transformed occurrences of one source, so source file counts alone do not prove a graph fits.

For this representation, require all of the following before allocation of the corresponding compact array:

| Array or record | Required checked bound |
| --- | --- |
| Vertex count | `V <= UINT32_MAX` |
| Face IDs and three face uses | `3 F <= UINT32_MAX` |
| Edge IDs and two vertex-edge uses | `2 E <= UINT32_MAX` |
| Point-reference slices | `R <= UINT32_MAX` |
| Boundary-incidence slices | `B <= UINT32_MAX` |

The effective face bound is 1,431,655,765, even though a face ID by itself could represent more. The effective edge bound is 2,147,483,647. Multiply and add in a wider checked type before narrowing. Check actual edge counts after sorting; a conservative bound of three edges per face can reject inputs that would fit. The self-test covers each boundary without allocating billions of elements.

Define an explicit overflow policy. A wide fallback preserves the accepted input domain. A clear failure is simpler but changes behavior for large expanded source-instance sets. A merge must select and test that policy. Do not silently truncate or use `UINT32_MAX` as a missing-value sentinel if the format also permits that value as an offset.

## Implementation and merge gates

Use an immutable graph owner with slice accessors. Keep owner lifetime explicit in graph/result snapshots. Do not store a reconstructed span after the owner can change. A slice survives vector relocation only when its new owner is correctly attached; it does not by itself supply ownership.

Build directly into compact arrays. Converting a completed wide graph keeps both representations during conversion and can increase peak memory. The prototype measures direct construction and releases scratch at the same stages as production.

Start with incidence and graph IDs. Keep analysis geometry and finding IDs wide at API boundaries. This reduces the migration scope and avoids changing source identity or intersection contracts. Later geometry-ID narrowing would reduce the three vertex-ID payloads by 12 bytes per analysis face. With the current inline reference and normal eight-byte alignment, the complete face record would instead decrease from 48 to 40 bytes, saving eight bytes after padding. A separate ID array could retain the full 12-byte payload saving. These are layout calculations; this experiment does not measure either geometry variant.

For a merge, require unchanged complete detector results, ordering, source references, status, truncation, cancellation, and exports in both topology modes. Check retained/peak memory in an application run that requests all topology families. Test removed-source and cache ownership across late worker completion. The prototype is an implementation decision input, not a ready production patch.

## Reproduction

Configure the repository with `vs2026-vcpkg` and `-DWOBY_BUILD_SIMPLIFICATION_PROBES=ON`. Build `woby_simplification_16_probe` in Debug and Release. Run the `woby_simplification_16_contract` CTest entry. Then run [the measurement script](../experiments/simplification-16/run.ps1) with an idle machine. It uses BusGameMap, the topology and attached-fin sample meshes, and Powerplant. The first three inputs use five samples per topology mode; Powerplant uses three. Use `-Model` and `-LargeModel` to supply different real-model paths.

The current executable has all four backends. The preserved source and original raw file retain the three-backend trial. Use [the proved runner](../experiments/simplification-16/run-proved.ps1) for the controlled construction comparison: production versus proved on BusGameMap, and all four backends on Powerplant.

Use [the envelope runner](../experiments/simplification-16/run-envelope.ps1) after the proved runner to repeat production/proved on Powerplant with the exact same executable. It rejects a changed source or executable hash and checks counts/capacities against the prior trial.

Requested live, peak, and cumulative C++ allocation bytes come from a global allocation wrapper. They exclude allocator overhead, source mesh, already-built shared analysis geometry, process working set, and RSS. The graph array capacities and analysis-geometry capacities are reported separately. The same allocation instrumentation is active in all variants.
