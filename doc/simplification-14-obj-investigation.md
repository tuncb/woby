# Item 14: direct OBJ corner access

Baseline: `46a27585c5bc8638a8bde5d601e5cead71376631`. The original assessment
lists this proposal under “Larger alternatives and limits”, after local queries.
The research target does not change the application loader or renderer.

## Decision

Reject direct corners as the default replacement for the measured corpus.
All five files need more GPU surface payload, even before representative
storage. Preserving normal and marker identity also retains tuple indexing.
The narrower indexed-tuple layout has an eligible GPU experiment: powerplant
can save 131.35 MiB, or 27.35% of surface payload, while retaining draw indices
and render IDs. That layout is unimplemented and needs a GPU/integration trial
before merge. For no-UV meshes, also compare a simpler 24-byte GPU vertex.

## Current boundaries

The production parser writes caller-owned double position, normal, and UV
arrays. The loader moves positions into immutable source storage after rebasing.
It triangulates in source space and maps each `(position,normal,UV)` tuple to a
globally assigned first-use render ID. Equal attribute values with different
indices remain distinct. The primary position table handles the first tuple;
only alternative tuples use the secondary hash table. A no-normal/no-UV file
uses direct primary indexing, without stored tuple keys.

Each new tuple produces a 32-byte `Vertex`. A source-position mapping is
retained only when identity mapping fails. Render triangle indices and source
triangle indices remain distinct. The source data supplies exact/source-index
topology. Render vertices supply authored seams, generated normals, picking,
annotations, UV analysis, dimensions, display preparation, and points.

If any render vertex has a missing, invalid, or almost-zero normal,
`finalizeMesh` calls `generateSmoothNormals`, replacing all normals. That
algorithm accumulates normalized face normals at render tuple IDs. Grouping
only by source position merges UV seams and changes its result. An equivalent
corner path needs a tuple-to-normal identity map even when markers are hidden.
Preserving complete authored normals is the simplest initial eligible subset.

Markers have an additional identity rule: `prepareSceneMesh` emits each render
vertex once per node, in first encounter order. A tuple shared across nodes can
therefore produce one marker entry in each node. The point hierarchy stores
original marker ID, position, and a cold render-vertex mapping. Using source
position IDs would collapse seam markers and change order. Using corner IDs
would duplicate markers at every use. Neither is equivalent. A direct path
that preserves current markers needs tuple ID assignment and representative
corners. Retaining only a GPU corner stream cannot supply those CPU contracts.

## Memory and break-even

Let `S`, `N`, `U`, `C`, and `R` be declared positions, normal records, UV
records, triangulated corners, and unique render tuples. Common double source
positions and source triangle connectivity are excluded below. Unused attribute
declarations count if the arrays are retained; they can materially change the
result. The probe also counts used attribute indices and reports a lower payload
bound with unused GPU attributes removed. Reaching that bound needs attribute
compaction and corner-index remapping; that extra work is outside packing timing.

| Surface payload | Bytes |
| --- | ---: |
| Current GPU vertices and render indices | `32R + 4C` |
| Separate float arrays and 12-byte corners | `12S + 12N + 8U + 12C` |
| Direct plus representative corners only | `12S + 12N + 8U + 12C + 4R` |
| Direct plus corner marker IDs and representatives | `12S + 12N + 8U + 16C + 4R` |

The bare direct format is smaller only when
`32R > 12S + 12N + 8U + 8C`. The representative-only format needs
`28R > 12S + 12N + 8U + 8C`. The format that also retains tuple IDs per corner needs
`28R > 12S + 12N + 8U + 12C`. All can lose on highly reused geometry.
For a grid with `C/R` near six, the bare extra corner indices alone cost
48 bytes per render vertex before attribute arrays; current vertices cost
32 bytes. Attribute-heavy does not automatically mean seam-heavy enough.

Tuple IDs per corner are a simple sufficient format, not a required retained
array. Complete authored normals let the application release those IDs after
it builds representatives and per-node point ranges. Generated normals need a
normal record per tuple; rewrite the corner normal indices to refer to those
records, then release the temporary tuple-ID array. The existing node marker
lists, 16-byte point packets, and proxies remain additional storage. Those
arrays must be counted in an application prototype. Discarding source position indices requires CPU
topology to access the position member of each corner; adding a second source
index stream instead forfeits that saving.

Current incremental CPU surface storage is `32R + positionMapBytes + 4C`.
An ideal direct representation can reuse the position member of each corner as
source connectivity, so its incremental storage is
`12S + 12N + 8U + 8C`, or `12S + 12N + 8U + 8C + 4R` with representatives.
Keeping tuple IDs per corner adds another `4C`. If normal generation runs,
replace `12N` with `12R` in the equivalent representations.
This requires new accessors. Adding direct buffers while retaining the current
`Mesh` for CPU consumers is additive storage, not a saving.

## Narrower indexed layout

Keep first-use render IDs and the existing `4C` index stream. Replace each
32-byte GPU vertex with a 12-byte `(position,normal,UV)` tuple and fetch from
separate float arrays. This preserves indexed vertex reuse and marker IDs.
It retains the tuple mapping pass; it is a layout alternative to the original
mapping-removal proposal. Its GPU surface payload is
`12S + 12N + 8U + 12R + 4C`, which is smaller when
`20R > 12S + 12N + 8U`. Corner reuse no longer sets the break-even condition.

For regenerated normals, substitute `12R` for the normal array. A full 12-byte
tuple is then smaller only when `8R > 12S + 8U`. An 8-byte `(position,UV)` tuple
can use the render ID as its implicit generated-normal index. That payload is
`12S + 8U + 20R + 4C`, and is smaller when `12R > 12S + 8U`.
Removing unused attributes can improve either calculation, but requires
compaction/remapping work. These layouts are unimplemented and untimed.

A GPU-only experiment must retain original tuple indices and float attribute
arrays, or reconstruct equivalent arrays after the loader discards that data.
Keeping existing CPU vertices adds CPU storage. Replacing them requires the
same CPU accessor migration described above. Vertex shader indirection, cache
behavior, byte-capacity limits, and shader layout remain GPU/integration gates,
even when the payload calculation is favorable.

## Experiment

The probe parses and triangulates each real file once. Its current control uses
the production tuple-table helper extracted with checked source anchors.
It gathers the same 32-byte vertices and writes source/render indices. The
candidate converts separate attributes to floats and writes 12-byte corners;
a second candidate assigns the same tuple IDs and representatives needed to
retain the current marker contract. Per-node marker preparation remains outside
the timer, as it does in the current control.
Five runs rotate the order. Every corner's position, authored normal, and UV
must match the control bit for bit; marker IDs must match in the compatible
candidate. Validation is outside timers. CPU sequential fetch loops use the
same output checksum, to bound the accessor locality trade-off.

Measurements exclude normal generation, bounds, group/annotation/point
preparation, GPU upload, rendering, cancellation publication, and completed
application load time. The bare variant has changed marker semantics and is
not a merge-compatible timing control. Neither CPU gather timing nor payload
size proves a GPU speedup.

The control is an isolated representation builder using the current helper,
not a timed copy of the complete production mapping loop. It excludes node
records, per-node finite-UV flags, and progress/cancellation checks. Temporary
tuple-table destruction is included. Position-map allocation begins only when
identity indexing fails. Authored normals use direct float casts without extra
range checks or normalization, as production does. UV conversion is
`float(U), 1-float(V)`. The primary table has `S` entries and tuple keys reserve
`min(S,C)`; the inspected corpus stays below the production vertex-byte capacity
clamp. These differences limit application timing predictions. They do not alter
the reported exact payload sizes or the verified tuple/attribute outputs.

The contract covers distinct but equal position/attribute declarations, unused
positions, missing attributes, seams, cross-group reuse, triangle order, and
the position-only shortcut. Filesystem fixtures are not needed; it uses the
memory parser. The Release contract and all five real-model checks passed.

## Corpus and timing results

The serial Release run used a Ryzen 7 5800H and MSVC 19.51.36243.0 with
`/O2 /Ob2 /DNDEBUG`. Each model ran in a fresh process; the five packing
samples rotated candidate order inside that process. SHA256 pre-reads warmed
the input. These are repeated representation builds, not cold complete loads.

| Model | Positions S | Normals N | UVs U | Render tuples R | Corners C |
| --- | ---: | ---: | ---: | ---: | ---: |
| BusGameMap | 535,893 | 110,180 | 490,283 | 547,469 | 3,163,626 |
| powerplant | 5,984,083 | 794,066 | 0 | 10,953,627 | 38,277,738 |
| san-miguel | 5,933,233 | 6,721,532 | 2,221,669 | 9,021,391 | 29,942,097 |
| Bennu | 8,933,524 | 0 | 0 | 8,933,524 | 53,600,508 |
| BearTrap | 37,890,645 | 0 | 38,414,285 | 38,414,391 | 227,315,958 |

Bennu and BearTrap require generated normals. The other three have complete
valid authored normals. Every file has nonidentity source-position mapping;
even Bennu's first-use render order differs from source declaration order.
Used attributes almost equal declared attributes: Bus has 28 unused positions
and san-miguel has two. Compaction therefore does not rescue these corner
layouts. Corners per render tuple range from 3.32 to 6.00.

Median packing times in milliseconds:

| Model | Current helper/control | Bare corners | Tuple/marker compatible |
| --- | ---: | ---: | ---: |
| BusGameMap | 82.622 | 32.589 | 78.300 |
| powerplant | 2,796.223 | 354.113 | 2,391.064 |
| san-miguel | 1,981.498 | 338.979 | 1,769.478 |
| Bennu | 1,356.649 | 503.173 | 1,364.822 |
| BearTrap | 10,560.306 | 2,298.800 | 6,382.948 |

Bare packing cuts this boundary by 60.6–87.3%, but changes identity semantics.
Keeping tuple IDs cuts it by 5.2% on Bus, 14.5% on powerplant, 10.7% on
san-miguel, and 39.6% on BearTrap; Bennu is 0.6% slower. BearTrap's current
samples span 10,442.693–11,140.412 ms, and compatible samples span
6,275.257–7,317.401 ms. Full samples remain in the raw results.
These savings exclude normal generation and all finalization/preparation.
For Bennu and BearTrap, the timed control and candidate retain pre-finalization
zero normals. The generated-normal storage calculations below are equivalent
format bounds; their extra normal generation/remapping was not timed.

Median CPU fetch times in milliseconds:

| Model | Unified vertices | Separate corner arrays | Increase |
| --- | ---: | ---: | ---: |
| BusGameMap | 62.200 | 102.173 | 64.3% |
| powerplant | 743.771 | 1,217.798 | 63.7% |
| san-miguel | 592.859 | 974.675 | 64.4% |
| Bennu | 1,459.575 | 1,860.709 | 27.5% |
| BearTrap | 4,648.640 | 8,962.471 | 92.8% |

Each walk includes an eight-word FNV checksum per corner. This is an accessor
plus checksum cost, not isolated memory bandwidth. Unified walks always precede
separate walks, so cache/order effects are not eliminated. Generated-normal
fetches are not included on Bennu/BearTrap. Actual GPU behavior remains unmeasured.

## Corpus memory decisions

GPU surface payloads below are MiB (`2^20` bytes). Generated-normal cases use
one float3 normal per render tuple. The corner columns are the equivalent
surface with and without a four-byte representative record per tuple. If those
representatives stay only on CPU, use the first corner column; it still loses
on every file. These figures exclude common node/point packets, allocator
overhead, buffer retention, and driver allocations.

| Model | Current | Corners, no reps | Corners with reps | Indexed 12-byte tuples |
| --- | ---: | ---: | ---: | ---: |
| BusGameMap | 28.78 | 47.34 | 49.43 | 29.47 |
| powerplant | 480.30 | 515.62 | 557.41 | 348.94 |
| san-miguel | 389.53 | 504.43 | 538.85 | 379.23 |
| Bennu | 477.10 | 817.88 | 851.96 | 511.18 |
| BearTrap | 2,039.46 | 3,767.74 | 3,914.28 | 2,473.08 |

The indexed tuple alternative saves 131.35 MiB/27.35% on powerplant. It saves
only 10.30 MiB/2.64% on san-miguel and increases payload on the other three.
The 8-byte tuple with implicit generated-normal index makes Bennu exactly tie
the current payload, and leaves BearTrap at 2,326.54 MiB, 14.1% larger.
A 24-byte position/normal GPU vertex for files with no UVs would give 396.73 MiB
on powerplant (83.57 MiB/17.4% saved) and 408.94 MiB on Bennu
(68.16 MiB/14.3% saved). This simpler format also preserves indexed reuse.
These indexed/24-byte layouts are report-only calculations. No packing time,
shader timing, or CPU memory saving was measured for them.

Ideal incremental CPU surface payloads with representatives are:

| Model | Current, MiB | Equivalent corners/reps, MiB | Change |
| --- | ---: | ---: | ---: |
| BusGameMap | 30.86 | 37.36 | +21.0% |
| powerplant | 522.08 | 411.39 | -21.2% |
| san-miguel | 423.94 | 424.63 | +0.2% |
| Bennu | 511.18 | 647.49 | +26.7% |
| BearTrap | 2,186.00 | 3,047.14 | +39.4% |

The ideal candidate reuses corner position indices as source connectivity and
releases the temporary corner tuple-ID stream after normal/marker preparation.
The measured compatible builder retains that stream. This table therefore
describes a feasible representation bound, not its measured heap footprint.
It excludes common double source positions/source connectivity, per-node marker
lists/point packets, vector excess capacity, and allocator overhead. Powerplant's
CPU benefit accompanies a worse nonindexed-corner GPU payload; it is not a
general application memory saving. Adding GPU tuples while retaining current
CPU vertices has no such CPU benefit.

## Smaller CPU capacity opportunity

The current control also records actual `std::vector<Vertex>` capacity. Excess
beyond `32R` is 7.82 MiB on Bus, 76.62 MiB on powerplant, 132.09 MiB on
san-miguel, zero on Bennu, and 562.18 MiB on BearTrap. Production uses the same
reserve/growth policy, and finalization/publication retains the vector without
shrinking it. This is allocation capacity, not process working set. A one-time
trim before publication can preserve all IDs/accessors, but can copy the entire
array and temporarily retain both old and replacement allocations.
The separate capacity probe does not change the loader or the frozen mapping
probe. It creates finite synthetic vertices at each measured size/capacity pair,
uses the same MSVC vector implementation with a tracking allocator, then times
one `shrink_to_fit` call. Initial allocation/filling and full byte/checksum
validation before/after are outside the timer. Five repetitions use fresh buffers.
Allocator instrumentation, heap state, and synthetic filling differ from a real
load; these are isolated trim times, not completed-load overhead measurements.

| Model | Freed Vertex capacity, MiB | Median trim, ms | Full range, ms | Peak requested Vertex storage, MiB |
| --- | ---: | ---: | ---: | ---: |
| BusGameMap | 7.82 | 4.502 | 4.258–6.051 | 41.24 |
| powerplant | 76.62 | 93.549 | 83.364–106.931 | 745.17 |
| san-miguel | 132.09 | 69.783 | 68.594–79.319 | 682.71 |
| Bennu | 0 | no allocation | timer-resolution no-op | 272.63 |
| BearTrap | 562.18 | 300.803 | 293.613–387.762 | 2,906.81 |

Changed vectors make exactly one replacement allocation and retain exact-size
capacity. On BearTrap, requested Vertex storage goes from 1,818,750,944 to
1,229,260,512 bytes; both overlap at 3,048,011,456 bytes. Thus freeing 562.18 MiB
requires an additional 1,172.31 MiB of Vertex storage during the copy. This peak
excludes the rest of the scene/parser, allocator overhead, and process memory.
It must not be described as a load-peak saving. Every byte and checksum matched;
Debug and Release trim/no-op/empty contracts passed with warning-free builds.

This supports a small worker-side trim trial. For example, a 64 MiB excess
threshold selects powerplant, san-miguel, and BearTrap and avoids the tiny-file
copy; that threshold is a proposed policy, not a measured optimum. Release
parser/index-table temporaries before copying where possible, and trim the
owned construction draft before `publishMeshGeometry`. Calling
`editMeshGeometry` after publication copies the full geometry, adding indices
and other arrays to the temporary peak. MSVC's inspected `shrink_to_fit` provides
a strong guarantee and can throw on allocation failure; an optional optimization
can retain the original capacity on `std::bad_alloc`. Check cancellation before
and after a large trim. Validate real loader peak private commit, progress/
cancellation, generated normals, mixed primitives/freeform append, publication,
and unchanged source/render/marker IDs before merging the loader change.

The trim raw results are in
[simplification-14-capacity-results.json](simplification-14-capacity-results.json).
Reproduce with `experiments/simplification-14-capacity/run.ps1` after building
its optional Release target. Input pairs are taken directly from the main raw
results; their SHA256 is recorded. The shared metadata includes this probe's
source, executable, input hashes, and run timestamps.

Raw model hashes, all samples, used-attribute counts, and exact byte values are
in [simplification-14-results.json](simplification-14-results.json). Indexed
layout calculations are in
[simplification-14-derived-payloads.json](simplification-14-derived-payloads.json).
The shared [run metadata](simplification-12-14-15-run-metadata.json) records
machine, compiler flags, timestamps, source/generated-copy hashes, and executable
hashes. Reproduce with `experiments/simplification-14/run.ps1`, then
`experiments/simplification-14/summarize.ps1`; use fresh output paths for reruns.

## GPU and implementation gates

Direct non-indexed corner draws normally execute the vertex path for every
corner, with three index loads and separate attribute fetches. The indexed
format can reuse transformed vertices. The reuse loss and extra indirection
need actual GPU timestamps; CPU fetch timing is not a substitute. A corner
representation can retain a tuple-ID index stream and one representative
corner per tuple to recover indexed reuse, but then tuple mapping remains and
the representation has additional indirection.

An eligible implementation must define mixed `f`, `l`, `p`, and freeform
append, missing/invalid normals, zero/NaN UV handling, unused attributes,
capacity checks for every separate GPU buffer, and source/render origin
behavior. The current 32-bit limit is a byte limit per buffer; a 12-byte corner
buffer can exceed it before a four-byte triangle buffer does. Splitting buffers
does not authorize silently increasing supported primitive counts.

Require matching source IDs, tuple IDs, node ranges, marker order, generated
normals, annotation fingerprints/attachments, both topology modes, UV analysis,
CPU picking, dimensions, overlays, transparency, and export. Measure first
completed display, analysis-ready latency, peak private commit, retained CPU/GPU
storage, and CPU/GPU frame time on both supported backends. Until those checks
pass, an eligible payload calculation justifies an experiment, not a merge.
