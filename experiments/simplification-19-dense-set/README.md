# Separate single-key dense-set investigation

This probe does not change the item 19 map adapters, their generated source,
their measurements, or production behavior. It tests one alternative:
`unordered_dense::set` owns the insertion-ordered contiguous keys once.

The three substituted production files are `mesh_comparison.cpp`,
`mesh_duplicates.cpp`, and `mesh_topology.cpp`. Their consumers read only
`keys.size()` and `keys[id]`. None erases or mutates a retained key. Generated
copies change exactly one include per file. The baseline include names a
generated custom header. Its checked substitutions add the same tracking
allocator to both production vectors and rename its type/hash to avoid a name
collision. Hash/probing bodies stay unchanged. CMake checks each include anchor
and registers source changes as configure dependencies. The runner checks that
the generated files differ only in that include before and after the campaign.

`DenseSetKeys` owns the set. Its read-only accessors derive their result from
`values()` on each access. It stores no span, pointer, or reference to another
owner. Default copy/move therefore uses the library's container ownership.
Borrowed key references expire at unique insertion/reserve, as with the old
vector. Hits preserve the key address and do not allocate. The default append
strategy reserves one extra key slot and keeps a spare after unreserved growth,
with geometric reserve rather than one-slot growth. This extra capacity is
counted. A full-capacity owner first searches for a hit; this also covers copies
whose vector lost the spare. The micro also tests find-before-insert: it avoids temporary hit
insertion and needs no spare, but hashes/looks up each miss twice.
No erase API is exposed by the facade: dense erase moves the final value and
would change IDs. The lookup table is public for procedural probe access;
production callers must use the free insertion/reserve functions.

The set hash calls the unchanged `analysisKeyHash`. It has no `is_avalanching`
marker, so the library keeps its default hash mix. Floating inputs must satisfy
the production finite-coordinate boundary; signed zeros compare and hash alike,
and the first inserted key keeps its original bits.

The default bucket has a 32-bit value index. Its probe capacity limit is the
smaller of the production `SIZE_MAX >> 2` limit and `set::max_size()-1`.
Unsupported reserve/unique insertion throws before the library operation.
This is a probe-domain restriction, not preservation of the full wide API.
A production substitution must retain the custom index as a wide fallback,
or separately establish that the big bucket meets the domain and memory needs.
The micro also measures the packed big bucket, whose value index is `size_t`.
It does not run a third complete detector campaign.

With `vs2026-vcpkg`, enable `WOBY_BUILD_SIMPLIFICATION_PROBES` and
`WOBY_BUILD_HASH_BENCHMARKS`. Build the following in Debug and Release:

- `woby_analysis_index_dense_set_probe`
- `woby_benchmarks_dense_set_custom`
- `woby_benchmarks_dense_set`

Run CTest `woby_analysis_index_dense_set_contract` and, on Windows,
`woby_analysis_index_dense_set_exit_contract`. The contracts compare
all stored keys and IDs through growth/reserve/copy/move, reused moved-from
owners, signed-zero/finite extremes, and capacity rejection with unchanged
cardinality. The capacity preflight precedes index allocator requests. The
exception runtime can allocate through the normal allocator; this is outside
the index ledger.
They cover every tuple arity/type in the three generated modules.
The exit contract runs both real detector binaries on one OBJ fixture in one
unique system temporary directory. It verifies one nonzero allocation summary,
zero live requested bytes at exit, and equal count vectors. It removes the
fixture on success and failure. A function-local reporter is registered at the
first tracked allocation; the initial unused inline-global reporter failed to
emit in Release, so its partial pilot is preserved separately and excluded.

After an exclusive timing window is granted, run `run.ps1`. It refuses existing
output paths. The micro uses seed 417, double/size_t triples at 4,096/524,288
entries, nine samples, one insertion and three hit passes. Four backend orders
rotate; custom is first three times and each alternative twice. Construction/destruction and full
key verification are outside time; construction belongs to memory accounting.
The same tracking allocator measures exact index requested live/peak/traffic
for the extracted custom implementation and all sets. It counts values/bucket
rebinds, excludes input keys, allocator overhead and process memory, and affects
both micro and detector timings through allocation-event counters only. Other
application allocations use the normal allocator and are untracked.

Each BusGameMap/Powerplant backend runs in two fresh processes with three runs
per process. The second pair reverses order. The existing harness loads before
timing and groups automatic detector stages. Every `computeComparisonStages`
call receives default empty `analysisInputs`; no application analysis cache is
retained or reused between calls. Report first and later repetitions separately
when their behavior differs. Allocator, OS, and input-data warmth can differ. The
runner validates every count field and stage/header layout. One stderr JSON
summary at process exit reports aggregate requested index peak/traffic/calls
over all three runs and requires zero live bytes. It does not measure process
memory or a per-detector peak. The full pipeline
uses the default append/spare strategy. Big-bucket and find-first alternatives
are micro-only. Complete findings,
export, cancellation and source-order integration remain production merge gates.
Record Debug/Release warning-free builds and actual contract output before
interpreting timings.
