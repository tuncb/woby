# Item 10: exact predicates for self-intersections

The [production adoption](#production-adoption) section records the implemented
follow-up. The investigation and its original measurements below remain tied
to the stated baseline commit.

This investigation applies to production commit
`46a27585c5bc8638a8bde5d601e5cead71376631`. The application algorithm is
unchanged. The candidate and its contracts are in
[the isolated probe](../experiments/simplification-10/README.md).

**Decision: implement the guarded classifier in an isolated source file.**
The measured detector time falls by 70.5–74.1% on the bounded full model
and by 71.0–85.6% on complete real-model prefixes. An unrestricted adaptive
predicate replacement is unsafe. Keep the rational fallback and the
production filters. Merge requires the production port and checks listed
below; the experiment does not change application behavior.

## Production contract and cost

`trianglesSelfIntersect` rejects non-finite coordinates. It treats the
supplied finite doubles as exact coordinates. A collapsed triangle does not
make a finding. Closed geometric contact makes a finding unless its entire
intersection is an allowed shared vertex or edge. A shared feature requires
both matching source topology IDs and matching coordinates. Matching
coordinates alone are insufficient in original-index mode. Duplicate faces
make findings even if all their IDs match.

The existing floating point filters only prove disjointness. They cover
plane separation, projected separating edges, ordinary shared-edge
neighbors, and some shared-vertex neighbors. Every remaining pair converts
18 double coordinates to rational values, constructs two normals, and
clips up to six segments. Clipping divides rational values, constructs
points, and tests those points against the allowed feature. The finding
stores the original six triangle coordinates. It does not use any of the
constructed points.

The broad phase, source scope, geometry ownership, finding limits, work
limits, cancellation, and display data do not need a change for item 10.
There is no retained intersection-point array to remove. This opportunity
concerns narrow-phase CPU work and temporary allocation traffic.

## Candidate

The probe keeps the current disjoint filters. For a remaining pair, it
finds a nonzero two-dimensional projection for each triangle with exact
orientation signs. All zero projections indicate a collapsed triangle.
It selects the first nonzero projection, rather than the largest projected
area. Every sign is exact within the guard. A poorly conditioned projection
can require more expansion work. Selecting the largest approximate area
could reduce that work, but would need its own safe range checks and was
not required for this prototype.

It classifies two shared vertices directly. Noncoplanar triangles can only
meet on their shared edge. Coplanar triangles overlap beyond that edge
exactly when their other vertices are on the same side. Three shared
vertices indicate duplicate noncollapsed faces.

With zero or one shared vertex, it tests the six original triangle edges:

- Same nonzero plane signs reject a segment.
- A zero endpoint plane sign permits a projected point-in-triangle test.
- Opposite plane signs permit three tetrahedron orientation signs to
  classify the plane crossing against the triangle edges.
- Two zero plane signs use closed two-dimensional segment and containment
  tests. Proper crossings are outside an allowed shared vertex. Other
  segment intersections contain an original endpoint. These endpoint tests
  also find collinear overlap beyond the shared vertex.

Each test returns a boolean. It constructs no intersection coordinate and
performs no rational division in the adaptive path. The allowed contact
rules remain explicit. A generic closed-triangle overlap function would
not satisfy those rules.

The adaptive orientation implementation comes from the installed CDT
`predicates.h`, not a new geometry library. Its expansions use fixed
`std::array` storage. The
[adaptive predicate paper and source](https://www.cs.cmu.edu/~quake/robust.html)
explain why exact determinant signs can be obtained with adaptive work.
That result does not remove range and compiler requirements.

## Numerical range and compiler requirements

An unrestricted call to the installed adaptive functions is unsafe. Their
first comparison can accept `det == error_bound == 0`. Product underflow
can therefore return zero for a nonzero exact determinant. Very large
products can overflow. The self-test records direct counterexamples at
binary exponents -1074, -530, 510, and 1000.

The candidate accepts zero coordinates and nonzero coordinates with
magnitude in `[2^-200, 2^200]`. Other finite coordinates use the current
rational algorithm. Within this conservative interval, each input is a
multiple of `2^-252`; exact degree-three products are multiples of
`2^-756`. Differences are at most `2^201`. The orientation products and
the splitter products remain well inside normal double range. This keeps
the expansion arithmetic away from underflow and overflow. The guard is
deliberately narrower than the maximum useful range.

The candidate and oracle source files use `/fp:strict`. The generated
production wrapper uses `/fp:precise`, as in the production build. A pilot
showed that applying strict options to the existing filters causes a large
regression even when the candidate is never called. The separate opt-in
contract target also uses `/arch:AVX2`. It selects the installed header's
FMA implementation. Both paths need the same rational sign and triangle-pair
contracts. A production change must retain strict options for the adaptive
source and normal options for the filter source. Changing the guard or
those options requires new range tests.

Expansion arithmetic also assumes round-to-nearest. The contracts use the
process floating point environment and do not vary its rounding mode.
Strict compilation does not enforce that mode. A production worker must
verify `FE_TONEAREST` before this route, or use the rational fallback.
The guard keeps arithmetic away from subnormal results; it does not remove
the rounding-mode requirement.

Template emission also needs control. Other CDT users in `woby_core` can
instantiate the same header templates with different floating point or
instruction options. A linker can select one emitted body for multiple
users. Different FMA definitions also break the one-definition rule. The
probe includes the unchanged installed header under a separate namespace.
This makes its strict and FMA checks independent of the core's CDT
instantiations. A production change must retain that isolation or make the
options and definitions consistent across every instantiating source.

## Validation design

The build generates an instrumented copy of the unchanged production
translation unit. The copy can force the rational clipping path, bypassing
the disjoint filters. This is the semantic oracle for shared features.
The candidate uses a different orientation-based algorithm. A second
independent rational separating-axis oracle checks closed intersections
with distinct IDs.

The pair corpus includes all vertex permutations and triangle orderings,
shared vertices and edges, duplicates, coplanar containment, collinear
contact, T contact, near-coplanarity, collapsed triangles, signed zero,
subnormals, and very small and very large scales. Random cases use fixed
seeds. Direct orientation tests also compare raw adaptive results with
rational determinants, including deliberate near-cancellation inputs.
An independent integer sign oracle decodes binary64 mantissa and exponent
bits, aligns the coordinates to a common binary exponent, and evaluates
the determinants with `cpp_int`. It does not use floating point arithmetic
or the production double-to-rational conversion.

During each model frequency pass, the probe captures up to 20,000 pairs
that enter the current rational path. It compares each captured pair with
the forced clipping oracle. Every timed pass compares the complete
retained finding records, candidate count, truncation state, and finding
order. Result comparison takes place outside the timing.

The synthetic meshes run without either budget. The full BusGameMap run
uses default production budgets and reports truncation. A fixed first
25,000-face prefix runs without either budget in each topology mode. This
distinguishes complete comparisons from comparisons of a bounded result.

Both final Release contracts passed: each checked **30,256 triangle pairs,
12,000 orientation signs, and 1,000 rational separating-axis cases**. The
normal target reports `predicates_fma_path: false`; the optional AVX2 target
reports `true`. Both also record the unsupported-range counterexamples.
Debug and Release builds of both targets passed without warnings. The
timed campaigns checked all 29,715 real-model rational survivors and all
16,857 synthetic survivors against the clipping oracle. These counts are
below the capture cap in every case. Every retained finding and its order
matched the normal production API in every comparison. No measured
survivor needed the guarded rational fallback.

## Measurements

All samples, counters, allocation values, contract outputs, source and
binary hashes, and the compiler-scope pilot are saved in
[the tracked raw results](simplification-10-predicates-results.json).
Measurements used MSVC 19.51.36243.0, x64 Release `/O2 /Ob2`, on an AMD
Ryzen 7 5800H. The normal adaptive path was timed; AVX2 was used only for
the separate numerical contract. No other benchmark or build ran during
either measurement window.

Each campaign starts with an unmeasured frequency and oracle pass. It then
alternates the instrumented baseline and candidate order across repeats,
and runs the normal production API in each repeat. Finding comparisons and
allocation passes are outside the timer. The table compares median complete
`inspectIntersections` time against that normal API, including BVH creation,
filters, narrow phase, and report creation. It excludes OBJ loading,
analysis-geometry creation, topology welding, UI, and rendering.

Synthetic cases have five repeats. The model cases have three repeats.
BusGameMap has 1,054,542 render faces and 75,585,857 input bytes; the raw
metadata records its SHA-256. Its parts form one identity-transform analysis
source. The fixed prefixes use the first 25,000 faces of that source with
the same vertex values and face order. Original-index and exact-position
modes are separate workloads.

| Workload | Exact survivors / candidates | Findings; result scope | Production ms | Candidate ms | Time reduction |
|---|---:|---|---:|---:|---:|
| Curved 64×64 grid | 0 / 60,285 (0%) | 0; complete | 24.52 | 24.41 | within run variation |
| Flat 64×64 grid | 11,907 / 68,104 (17.484%) | 0; complete | 1,475.93 | 93.85 | 93.6% |
| 100 duplicate faces | 4,950 / 4,950 (100%) | 4,950; complete | 144.22 | 7.35 | 94.9% |
| Bus, original index | 11,771 / 538,195 (2.187%) | 10,000; pair-limit truncation | 3,357.09 | 990.05 | 70.5% |
| Bus, exact position | 12,402 / 576,805 (2.150%) | 10,000; pair-limit truncation | 3,950.78 | 1,021.36 | 74.1% |
| First 25,000 faces, original index | 4,470 / 212,998 (2.099%) | 4,230; complete | 839.62 | 120.75 | 85.6% |
| First 25,000 faces, exact position | 1,072 / 212,998 (0.503%) | 832; complete | 390.65 | 113.33 | 71.0% |

The full runs reach the default 10,000-finding limit before the
1,000,000-candidate limit. Their times and survivor percentages describe
only that deterministic bounded traversal. They do not measure all
intersections of the million-face model. The unlimited prefixes provide
complete real-data comparisons, but they are not a random model sample.

Final candidate ranges were 983.29–1,005.91 ms and 1,018.20–1,027.12 ms for
the two full modes; corresponding production ranges were
3,333.12–3,429.23 ms and 3,891.07–3,960.49 ms. Curved-grid ranges overlap:
23.92–25.34 ms candidate and 23.72–25.82 ms production. There is no measured
benefit in that zero-survivor case. Final instrumented-baseline medians
were close to production: 3,388.07 versus 3,357.09 ms in full original mode,
and 3,895.63 versus 3,950.78 ms in full exact-position mode. The main result
does not depend on using an artificially slow instrumented baseline.

The flat grid's 11,907 survivors all share one allowed vertex. A boolean
answer that preserves allowed-feature rules removes rational work even
when there are no findings. In the bounded full model, most survivors have
no shared feature: 11,691 of 11,771 in original mode and 12,269 of 12,402 in
exact-position mode. Thus the real-model gain also covers ordinary triangle
intersections.

### Allocation traffic and live memory

The same replacement global C++ allocator serves all timed variants, with
recording disabled during timings. Separate passes count requested payload
for `new` and `new[]`. Values exclude allocator headers and reservations,
aligned allocation, DLL-internal allocation, input/source geometry, and
stack storage. These are detector heap measurements, not process working
set. A normal-allocator application measurement remains a merge check.

| Workload | Allocation calls, baseline → candidate | Requested traffic MiB, baseline → candidate | Peak live / retained MiB, both variants |
|---|---:|---:|---:|
| Curved grid | 5 → 5 | 0.594 → 0.594 | 0.594 / 0 |
| Flat grid | 201,920 → 5 | 29.993 → 0.594 | about 0.594 / 0 |
| Duplicate faces | 34,678 → 9,928 | 6.306 → 3.134 | 1.455 / 0.864 |
| Bus, original index | 8,824,568 → 20,030 | 784.110 → 113.484 | 106.579 / 1.945 |
| Bus, exact position | 9,783,894 → 20,030 | 852.687 → 113.484 | 106.579 / 1.945 |
| Prefix, original index | 2,572,508 → 8,488 | 200.398 → 5.574 | 3.569 / 0.864 |
| Prefix, exact position | 1,078,026 → 1,688 | 85.505 → 3.130 | 2.527 / 0.171 |

MiB means 1,048,576 bytes. Only the flat-grid peak decreases, by **576
bytes**, from 623,853 to 623,277. Every other recorded peak and every
retained value is exactly unchanged. The full-model allocation traffic
falls by 85.5–86.7%, or 670.6–739.2 MiB per bounded inspection; the complete
prefix traffic falls by 96.3–97.2%. That is temporary allocation churn,
not an equal memory-capacity saving. BVH and report storage remain. Output
allocations also explain why the duplicate case does not reach five calls.

In this build, `sizeof(cpp_rational)` is 64 bytes and one rational triangle
occupies 576 bytes before any dynamic integer payload. The candidate uses
fixed expansion arrays instead. Their stack usage is outside these heap
measurements. There is no evidence here for a large peak-RAM reduction.

### Compiler-scope pilot

The first pilot applied `/fp:strict` to the whole generated production
wrapper. Its curved grid had zero survivors, yet the instrumented baseline
median rose to 36.66 ms versus 24.31 ms for production: a **50.8%
regression**. The candidate median was 38.16 ms, a **57.0% regression**.
Its favorable survivor-heavy results would not justify merging that build
layout.

The final build keeps `main.cpp` and `predicate.cpp` strict and the wrapper
precise. The curved-grid regression disappears, while both numerical
contracts still pass. The algorithm, installed header, production source,
and workload are unchanged between these campaigns. The pilot data,
measured CMake source, generated-wrapper hash, project hash, and executable
hash are in the tracked results. A later `CMAKE_CONFIGURE_DEPENDS` entry
only ensures that production source edits regenerate the wrapper; it did
not change the measured binary.

## Implementation and merge gate

The required production scope is small: retain the BVH and filters, replace
the rational survivor path with explicit orientation classification, keep
the guarded rational fallback, and isolate strict floating point options.
Keep the rational clipping oracle in test code during development. Do not
remove the production rational fallback until a broader range proof and
its tests exist.

Before a production merge:

1. Port the classification to a separate production source file. Give it
   explicit strict floating point options and isolated predicate template
   emission. Keep the current filter source at its normal options. Verify
   round-to-nearest on the worker or select the rational fallback.
2. Move the numerical and shared-feature corpus into application tests.
   Keep the independent integer sign oracle and rational clipping oracle.
   Run the supported normal and FMA paths, Debug and Release, without
   warnings. Check source/ID mismatch, signed zero, guard boundaries,
   unsupported scales, collapse, and full finding order.
   Add explicit values at each guard boundary and its next representable
   neighbor. The present raw random-sign corpus uses exponents -180 through
   180; scaled fixtures exercise broader ranges and fallback behavior.
3. Repeat complete and bounded model comparisons with the normal allocator
   in the application build. Include one zero-survivor mesh. Confirm that
   source transforms, cancellation, and detector limits retain the current
   contract. The probe shares the existing traversal but uses one
   identity-transform source, so it does not establish those integration
   checks by itself.

The measured gains are large enough to justify that implementation work.
The normal-allocator check is a practical confirmation, not a claim that
the experiment already changed production. The unrestricted library swap
and the whole-filter strict build are rejected. A narrower or wider guard,
a different projection choice, or another compiler needs separate
validation. No larger data structure change or new retained memory layout
is required.

## Production adoption

The application now uses the guarded adaptive classifier in
[`mesh_intersection_predicates.cpp`](../src/mesh_intersection_predicates.cpp).
The rational classifier remains in `mesh_intersections.cpp`. An unsupported
range or rounding mode selects that fallback; it does not clamp coordinates
or introduce a proximity tolerance. Non-finite coordinates still raise an
input error. An engaged `false` result from the adaptive classifier is a final
non-intersection decision, while a missing result requires rational work.

The range check reads IEEE-754 bits. It accepts signed zero and nonzero absolute
coordinates in `[2^-200, 2^200]`, including both endpoints. Subnormal values
cannot pass that check as zero. Adaptive arithmetic also checks `FE_TONEAREST`
on the calling thread. The detector checks rounding once for its synchronous
filter traversal; the traversal does not change that mode. A direct pair call
checks its own caller. The classifier checks again before expansion arithmetic.
Other modes bypass the floating-point filters and use rational classification.

Only the adaptive source uses `/fp:strict` on MSVC. Other compilers use
`-fno-fast-math -ffp-contract=off -frounding-math`. Its CDT template symbols have
a separate namespace, and the source does not use the core precompiled header.
The existing filters retain their normal compiler options. The optional
`WOBY_TEST_INTERSECTION_FMA` target compiles the actual production classifier
with AVX2/FMA; it is off by default and requires a compatible CPU.

Permanent application tests retain the independent integer sign oracle and a
frozen copy of the previous rational clipping classifier. Each normal/FMA
corpus checks 30,256 pairs, 12,000 orientation signs and 1,000 independent
rational separating-axis cases. Additional tests cover the guard endpoints
and their adjacent values, signed zero, source-ID/coordinate mismatches,
non-finite input, and separated planes whose determinant reaches the lower
guard's `2^-756` lattice. All four rounding modes are tested through both the
pair API and the detector traversal. Existing integration tests cover source
scope, transforms, topology modes, cancellation and result/work limits.

### Normal-allocator measurements

The [adoption runner](../experiments/simplification-10/run-adoption.ps1) compares
the production library with an uninstrumented rational baseline frozen from
investigation commit `dcee7da`. Its [source check](benchmarks/intersection-predicate-adoption-worker-guard/baseline-equivalence.json)
confirms that the frozen file matches that commit apart from its banner and
whitespace. Import and source preparation are outside the timer. Each result
comparison includes all findings, order, source metadata, status and limits.
Both variants use the normal allocator and run in alternating order.

The final seven-sample real-model campaign used the same Ryzen 7 5800H and
MSVC toolchain as the investigation. Medians are milliseconds:

| Workload | Rational baseline | Production | Change | Result |
| --- | ---: | ---: | ---: | --- |
| BusGameMap, original-index mode | 4,391.10 | 1,216.34 | -72.3% | 10,000 findings; pair limit |
| BusGameMap, exact-position mode | 4,742.45 | 1,222.39 | -74.2% | 10,000 findings; pair limit |
| First 25,000 faces, original-index mode | 1,119.79 | 138.12 | -87.7% | 4,230 findings; complete |
| First 25,000 faces, exact-position mode | 496.62 | 149.12 | -70.0% | 832 findings; complete |

All complete result comparisons passed. The bounded full-model results are
not presented as complete searches. Raw [model samples](benchmarks/intersection-predicate-adoption-worker-guard/model.json),
[synthetic samples](benchmarks/intersection-predicate-adoption-worker-guard/synthetic.json),
[provenance](benchmarks/intersection-predicate-adoption-worker-guard/metadata.json)
and [derived medians/ranges](benchmarks/intersection-predicate-adoption-worker-guard/summary.json)
are retained.

The first design checked rounding for every candidate pair. Its curved grid,
where the existing filters reject all 60,285 candidates, had a 7.7% median
regression. That [pilot](benchmarks/intersection-predicate-adoption/metadata.json)
and its measured source are preserved. The final design moves the filter
check to the detector call. The revised seven-sample curved-grid median was
30.76 to 27.94 ms (-9.2%), but its ranges overlapped substantially.

A [20-sample repeat](benchmarks/intersection-predicate-adoption-worker-guard/synthetic-repeat.json)
used the same unchanged executable to check that noise. Its curved-grid
medians were 26.63 and 28.29 ms (+6.3%), with ranges of 25.19–39.09 and
24.70–38.51 ms. Production was slower in 9 of the 20 paired samples; the median
paired ratio was 0.991. Thus this test establishes neither a stable speedup
nor the absence of a small regression in filter-only work. The explicit
per-pair rounding check has been removed from that path, but timing variation
remains. The 20-sample flat-grid and duplicate-face medians improved by 93.9%
and 95.9%, respectively. No further production changes were made to pursue
the noisy filter-only ranking.

The adoption measurement does not record process memory or VRAM. The new
classifier uses fixed local arrays and adds no retained geometry owner. The
original allocation-traffic measurements above remain prototype evidence.

### Adoption validation

The final full `vs2026-vcpkg` Debug build completed without warnings. All
129 CTest entries passed, with no skipped tests, in 563.78 seconds. This includes
the normal and FMA predicate corpora, detector integration, optional benchmark
contracts, graphics tests and slow workloads. The Release normal/FMA and
intersection integration contracts also passed.

The shared CMake helper applies strict settings to the core, FMA test and
standalone detector consumers. Generated MSVC projects were checked for those
settings in all four build configurations, including the nested dense-set
targets. The [validation record](benchmarks/intersection-predicate-adoption-worker-guard/validation/validation.json)
links the build/test evidence and records the final implementation hashes.
The optional-target wiring was completed after timing; the measured predicate
and traversal source files remain unchanged. Validation was performed on
Windows/MSVC; other compiler flags are configured but were not run here.
