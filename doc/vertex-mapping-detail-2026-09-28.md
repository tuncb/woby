# OBJ mapping: lookup, growth, and parallel gathering

Investigation of production commit `4cfcb11b0b62894316204f8b7bf306a7c059efe1`,
using all five models in `D:/temp/obj_tests` on 28 September 2026. Application
sources are unchanged. The optional research targets and their contract tests
are extended for this investigation.

**The most promising next change is to separate tuple lookup from vertex
construction for meshes with normal/UV attributes, retaining the existing
position-only shortcut.** In the research copies, serial separate passes reduce
mapping time by 36% on BearTrap and 7–12% on the other attribute-bearing models.
Applying the same split to Bennu makes it 12% slower. Eight-thread gathering
accelerates its own pass, but does not establish a useful overall advantage
over the serial split in these prototypes.

The [raw results](vertex-mapping-detail-2026-09-28-results.json) contain all
**60 successful runs**, three per model/variant, plus summaries and counters.
All output checks agree. These are measured prototype results, not a shipped
optimization or a measured change in first-display time.

## Measured mapping time

Median seconds, including initial mapping/output reservations and the entire
mapping loop. `Current` means the instrumented copy of the production algorithm.

| Model | Current | Separate passes, 1 gather thread | Separate passes, 8 gather threads | Time saved by serial split |
|---|---:|---:|---:|---:|
| BearTrap | 10.200 | 6.487 | 6.452 | 36.4% |
| Bennu | 1.084 | 1.213 | 1.234 | **−11.8% (slower)** |
| Powerplant | 2.488 | 2.204 | 2.178 | 11.4% |
| San Miguel | 1.794 | 1.572 | 1.562 | 12.4% |
| BusGameMap | 0.0635 | 0.0590 | 0.0631 | 7.2% |

For BearTrap, current mapping ranged from 10.161–10.207 s; serial split from
6.300–6.699 s; eight-thread split from 6.305–7.385 s. Full ranges for every
model/variant are in the JSON. BusGameMap's serial saving is only about 4.6 ms.
The eight-thread medians are close to serial on the large attribute-bearing
models, with overlapping ranges, and are worse on BusGameMap and Bennu.

Median complete CPU-load times, including parsing, source copying, mapping,
normal/bounds finalization and temporary release, were current → serial split:
BearTrap **19.375 → 15.627 s**, Bennu **2.584 → 2.762 s**, Powerplant
**3.565 → 3.144 s**, San Miguel **3.054 → 2.732 s**, BusGameMap
**0.196 → 0.187 s**. Parsing/I/O variation contributes to these totals; the
mapping comparison is the more focused result.

Initial reservations/primary-table initialization alone took 59.3, 10.3, 6.7,
6.9 and 0.7 ms respectively. Most work is inside the fused loop, rather than
these initial allocations. Mapping represents roughly 32–70% of CPU load
in the current-algorithm probes.

## Actual lookup frequencies

These are counts of **corners taking each lookup path**, not percentages of
unique vertices that happen to lie on seams.

| Model | Corners (millions) | Direct/primary path | Secondary hash path | Mean bucket slots per secondary lookup | Maximum slots |
|---|---:|---:|---:|---:|---:|
| BearTrap | 227.316 | 99.42% | **0.58%** | 3.32 | 195 |
| Bennu | 53.601 | 100% direct | **0%** | — | — |
| Powerplant | 38.278 | 66.19% | **33.81%** | 3.22 | 255 |
| San Miguel | 29.942 | 80.57% | **19.43%** | 3.59 | 209 |
| BusGameMap | 3.164 | 99.18% | **0.82%** | 3.40 | 98 |

For example, BearTrap has 37,890,645 new primary entries and 188,107,378
primary hits, versus 523,746 new secondary entries and 794,189 secondary hits.
Optimizing the secondary hash function alone therefore reaches only a small
part of BearTrap's corner loop. Powerplant and San Miguel are better candidates
for secondary-table work. The maximum probe lengths describe tails, not the
typical lookup; the full histogram is retained in the JSON.

For every primary hit with attributes, the current algorithm first loads the
render ID through the position table and then uses that ID to load/compare the
tuple key. Those dependent accesses, branching and interleaved vertex/index
writes remain even when hashing is avoided. Their individual costs are not
isolated by these measurements.

## Growth costs

Median milliseconds inside the counter-instrumented implementation. These are
nested inside its mapping-loop time and must not be added again. They include
instrumentation overhead and give an approximate size of the opportunity.

| Model | Vertex growth | Key growth | Secondary rehash | Vertex/key growth events | Live vertex/key bytes copied (MiB) |
|---|---:|---:|---:|---:|---:|
| BearTrap | 361.7 | 134.5 | 40.5 | 1 / 1 | 1,590.0 |
| Bennu | 0 | 0 | 0 | 0 / 0 | 0 |
| Powerplant | 120.9 | 44.2 | 238.8 | 2 / 2 | 627.8 |
| San Miguel | 116.2 | 44.5 | 114.6 | 2 / 2 | 622.4 |
| BusGameMap | 4.2 | 1.6 | 0.3 | 1 / 1 | 22.5 |

Copied bytes count the live elements carried into replacement arrays, not
physical memory traffic or initial population. Read/write traffic and allocator
work are additional. Secondary rehashing is separate: 17 growth events move
786,420 entries cumulatively on BearTrap; 20 move 6,291,444 on Powerplant; 19
move 3,145,716 on San Miguel; 11 move 12,276 on BusGameMap. The first event
allocates an empty table.

Growth matters, especially on the seam-heavy models, but eliminating vertex
growth alone cannot explain BearTrap's roughly 3.7 s mapping improvement.
Its measured vertex/key/rehash work totals only about 0.54 s in a roughly
10.9 s instrumented loop. Separating lookup and construction also changes the
hot loop and memory-access pattern. Pinning the remaining benefit on cache
misses or branch prediction would require hardware profiling.

Diagnostic mapping medians were 10.958, 1.121, 2.576, 1.879 and 0.0653 s,
2.8–7.4% above the timing controls. Instrumentation is demonstrably not free;
one BusGameMap diagnostic sample was 0.0927 s.

## Where the separate passes spend time

Median milliseconds. Allocation here is the **final vertex array allocation
and zero initialization**, after IDs are known; initial primary/index/key
reservations are outside these three subpasses.

| Model | Serial lookup + both index writes | Final vertex allocation | Gather, 1 thread | Gather, 8 threads |
|---|---:|---:|---:|---:|
| BearTrap | 5,091.5 | 296.1 | 1,069.0 | 274.3 |
| Bennu | 1,032.5 | 61.8 | 108.2 | 25.7 |
| Powerplant | 1,979.8 | 76.7 | 141.1 | 29.9 |
| San Miguel | 1,370.4 | 63.0 | 129.0 | 29.7 |
| BusGameMap | 47.0 | 3.7 | 7.8 | 2.2 |

The lookup/allocation columns come from `split1`; the last column comes from
`split8`. Separate stage medians need not sum to a median total. Gathering is
about 3.5–4.7 times faster with eight threads, including their startup/join
cost, but serial lookup/index writing still dominates.

There is an important compiler effect: the same lookup source compiles
differently in the two prototypes. MSVC inlines source-vector size and append
operations in `split1`, while `split8` calls them out of line for every corner.
Its serial lookup medians increase to 5,832.1, 1,137.8, 2,063.7, 1,463.0 and
56.4 ms, respectively. Thus the whole-prototype comparison includes changed
code generation as well as parallel gathering. It does not establish that
parallel gathering itself is unhelpful. A separate helper boundary for the
gather implementation is a worthwhile follow-up before selecting worker count.

## Memory effect

Median process peaks during CPU load, before annotation/group preparation.
MiB is 2^20 bytes. Working set and private commitment are distinct counters.

| Model | Peak resident, current → serial split (MiB) | Peak private commit, current → serial split (MiB) |
|---|---:|---:|
| BearTrap | 9,456.7 → 8,469.5 | 10,292.1 → 10,062.7 |
| Bennu | 1,692.0 → 1,794.4 | 1,995.9 → 1,995.9 |
| Powerplant | 1,646.2 → 1,506.8 | 1,886.6 → 1,535.4 |
| San Miguel | 1,587.2 → 1,324.6 | 1,779.2 → 1,374.8 |
| BusGameMap | 132.7 → 117.0 | 141.5 → 117.9 |

The serial split lowers BearTrap's observed resident peak by about **987 MiB**.
The final vertex count is known before allocating vertices, avoiding their
growth copies and spare capacity. BearTrap's retained vertex capacity drops
from 1,734.5 to 1,172.3 MiB. Output size and GPU payload are unchanged.
Key-vector growth still exists in the split prototype.

Bennu is the exception: the experiment adds about 102 MiB of tuple keys to a
path that normally needs none, raising resident peak and time. An unchanged
peak-commit counter does not negate that extra allocation; process peaks can
occur at different stages. Keeping the existing direct path avoids the penalty.

## What the current mapping step does

Mapping starts after parsing, triangulation and source-position validation.
It converts OBJ's three independent attribute indices into one render-vertex
index, without merging distinct OBJ indices that happen to have equal values.

1. Count triangulated corners and reserve render/source index arrays. Reserve
   vertices and tuple keys using the smaller of source-position and corner
   counts. Initialize a four-byte primary entry per source position.
2. Visit corners in shape/triangle order, validate the source position index,
   and append that position index to the source-topology index buffer.
3. Find the render ID. When both normal and UV arrays are absent, the primary
   entry directly holds the first-use ID. Otherwise, the first tuple at a
   position becomes its primary entry; repeated primary tuples compare against
   their stored key. Only alternative normal/UV tuples use the secondary hash
   table, with linear probing and growth at 75% occupancy.
4. On first use, gather position/normal/UV attributes into a 32-byte vertex.
   Missing attributes start at zero; UV V becomes `1 - V`. Append the render
   ID for every corner. Existing vertices reuse their IDs without gathering
   attributes again. Record each shape's contiguous index range.
5. Finalization subsequently checks/generates normals and calculates bounds.
   Those operations, parser cleanup, annotation preparation and GPU upload
   are outside the mapping timings in this report.

IDs are globally assigned on first use, including across shape boundaries.
Normal/UV seams intentionally split vertices while the separate source index
stream preserves position connectivity. There is no final value-compaction
pass. Source positions are now float triples, unlike the older mapping report.

## Current performance techniques

| Work | Current implementation |
|---|---|
| Reserve/fill primary table | Contiguous `std::vector<uint32_t>`, one entry per source position. |
| Assign IDs and look up tuples | One serial corner loop, using direct indexing for primary tuples and hashing only alternatives. |
| Secondary lookup | Contiguous tuple keys and 32-bit bucket IDs; power-of-two table, linear probing, 75% maximum occupancy before growth. |
| Gather attributes | Scalar C++ indexing into separate position/normal/UV arrays, only for new vertices. |
| Write topology | Two pre-reserved contiguous uint32 index streams: source and render. |
| Vertex/key growth | Standard vector growth when seams exceed the initial position-based estimate. |
| Parallelism | The viewer performs loading on a background worker, but this mapping loop uses one thread per model. RapidOBJ's earlier parallel parsing does not parallelize mapping. |
| SIMD | No explicit SIMD intrinsics, vectorized hash-table groups, or AVX-specific mapping kernel. The compiled production gather uses scalar float operations; the compiler copies a complete vertex with two 128-bit moves. |

Background execution preserves UI responsiveness; it does not divide one
model's mapping work across cores. The small-file prefetch path is separate
and does not apply to these five large OBJ files.

The [disassembly excerpts](vertex-mapping-detail-2026-09-28-codegen.txt) show
`movss` attribute loads, scalar `subss` for UV V, and `movups` for copying the
32-byte vertex. Using XMM registers here does not mean several corner lookups
or gathers execute together. The excerpts also document the split prototypes'
different inlining decisions. This is static inspection of compiled code,
not a sampled instruction profile.

## Recommended next steps

1. **Prototype the serial split in the application for meshes with attributes.**
   Preserve global first-use IDs, seams, source topology and corner order, and
   leave the direct position-only path as it is. Add progress/cancellation
   coverage for the new passes and remeasure through first completed display.
   The largest observed benefits are BearTrap's mapping time and resident peak.
2. **Keep lookup code generation stable while testing parallel gathering.**
   Move gathering behind a separate function boundary, then compare worker
   counts with a bounded worker budget. Gathering parallelizes safely over
   disjoint output records, but this prototype's extra serial-loop calls hide
   much of its benefit. The current measurements do not select eight workers
   as a production default.
3. **Investigate secondary-table growth on Powerplant and San Miguel.**
   Rehash costs are about 0.24 s and 0.11 s in the instrumented runs. A reserve
   strategy should be measured against memory cost; reserving for every corner
   would substantially overallocate on BearTrap. The final seam count is not
   known before mapping, so an exact reservation cannot simply be assumed free.
4. **Treat full parallel ID assignment and multi-corner SIMD as further research.**
   Global first-use order and shared positions prevent independent per-shape
   maps from being equivalent. A parallel solution needs partitioning and
   deterministic global numbering, with extra storage/passes to measure. SIMD
   gathering alone targets a smaller remaining stage; hashing and dependent
   primary/key accesses deserve profiling before choosing a vector kernel.

The benchmark is evidence for an application prototype, rather than a guarantee
of identical savings after integration. Adding stage timers/statistics also
changes function size and can affect compiler inlining: even the current-control
copy differs from the production object in vertex-append inlining. The observed
compiler sensitivity makes a fresh production build and first-display benchmark
an essential check before adopting a change.

## Measurement method

The `adaptive` target is the current production algorithm. `diagnostic` adds
path/probe counters and clocks only the relatively rare array growth and
rehashing operations. These counters alter hot-loop code and are not used as
the production timing control. No per-corner clock calls are used.

Two controlled alternatives keep the same serial ID assignment, both index
writes and node ranges, then allocate exactly the final vertex count and
gather attributes in a separate pass. `split1` gathers on the caller;
`split8` partitions output vertices into eight disjoint ranges and joins all
workers before normal generation. They preserve first-use order and seams.
On the position-only path they additionally store 12-byte keys, which production
does not need. Vertex allocation includes `vector::resize` value initialization;
parallel gather time includes thread creation and joining.

The separated timings answer what those passes cost **in that alternative**.
They cannot be subtracted from the fused production loop to assign exact costs
to its individual instructions: separating passes changes allocation, locality,
code generation and memory traffic. Lookup timing still includes source/render
index writes, validation and node ranges. There is no misleading additive
per-instruction breakdown of the original fused loop.

All measurements use fresh processes, run serially with rotated variant order.
The machine has a Ryzen 7 5800H (8 cores/16 logical threads), 64 GiB RAM and
Windows. Builds use MSVC 14.51.36231 through the VS 2026 `vs2026-vcpkg` preset.
The runner pre-reads each file once per round; RapidOBJ retains its production
unbuffered Windows reader. These are repeated-load measurements, not controlled
cold-storage tests. No build, test suite or viewer runs concurrently with the
sweep. CPU frequency/temperature are not controlled; three repetitions are not
a statistical confidence interval.

The loader receives an empty progress callback. Its progress checks remain,
but publication to the viewer UI is excluded. All variants finalize geometry
and then run the existing probe's synchronous annotation/group/point preparation
outside load timing, for validation. This does not represent the application's
new asynchronous annotation schedule. Full ordered hashes of vertices, both
index buffers, source points, node names/ranges and point lists, plus counts,
bounds and radius, must agree across variants and rounds. Hashing is untimed.
No GPU upload, completed frame or actual first-visible geometry is measured;
mapping savings alone must not be presented as a measured first-display saving.

No hardware cache-miss, memory-bandwidth or instruction-retirement counters
were collected. Locality/latency explanations are hypotheses based on the
algorithm and comparative measurements, not hardware-profile conclusions.

## Reproduction

See [probe instructions](../tests/vertex_mapping/README.md) for commands,
counter definitions and variant details. The source anchors are checked when
CMake generates the research copies; production source files are not patched.

## Validation

- All 60 Release model runs succeeded, with matching full ordered output
  fingerprints, counts, bounds and radius across variants and rounds. Counter
  totals/histograms and growth-copy counts also agree across repetitions.
- Release research targets built without compiler/linker warnings. Their
  expanded contract passed against four isolated fixtures covering seams,
  duplicate attribute values, missing normals, concave/reversed faces, negative
  indices, unused positions, multiple groups, Unicode paths, secondary growth,
  counter invariants, split timing accounting and repeated sweep output.
- `cmake --build --preset vs2026-vcpkg --config Debug` built the complete project
  without compiler/linker warnings.
- `ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure` passed
  **657/657 tests**, including slow and graphics tests, in **254.79 seconds**.
- Production application sources and behavior are unchanged. No tests were
  added for document content. Temporary contract fixtures use one unique root
  and are cleaned up on failure as well as success.

Local logs are `build/mapping-detail-release.log`,
`build/mapping-detail-debug.log`, `build/mapping-detail-ctest.log`, and
`build/mapping-detail-sweep.log`. Raw JSONL is `build/mapping-detail.jsonl`;
the linked JSON report retains all of its records plus computed summaries.
