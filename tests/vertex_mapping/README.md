# Mapping research targets

These optional targets generate copies of the current OBJ loader. They do not
change application loading behavior. Enable `WOBY_BUILD_OBJ_BENCHMARKS` through
the `vs2026-vcpkg` preset. Generated copies fail configuration if expected source
anchors change.

| Variant | Purpose |
|---|---|
| `adaptive` | Current production lookup and fused vertex construction; timing control. |
| `baseline` | Historical global tuple hash table, before primary-position lookup. |
| `hybrid` | Current lookup with the no-normal/no-UV direct shortcut disabled. |
| `diagnostic` | Current implementation plus lookup/probe counters and rare growth timers. |
| `split1` | Serial lookup and both index writes, then exact vertex allocation and serial attribute gathering. |
| `split8` | Same split, with up to eight threads gathering disjoint vertex ranges. |

The split variants retain first-use IDs, complete index-tuple identity, global
sharing across shapes, source topology, and normal generation. They store keys
even on the direct path, where production does not need them. Their allocation
pass includes value initialization by `vector::resize`; their gather timer
includes thread creation and joining. They are experiments, without production
progress/cancellation integration for the additional passes.

`diagnostic` counts corners taking each path, actual hash calls, bucket slots
examined per secondary lookup, secondary rehashes, and key/vertex vector growth.
The probe histogram bins are lengths 1 through 8, then 9–16, 17–32, 33–64, 65+.
Lookup slots include the terminal match/empty slot; they exclude rehashing and
the second placement search immediately after growth, which have separate
counters. Growth copied bytes count existing live elements, not physical memory
traffic; timings include allocation, copying, release, and the triggering push.
Rehash timings include counter overhead. Never treat instrumented loop time as
an unperturbed production measurement.

All variants still finalize normals/bounds. After load timing and memory
snapshots, the existing probe synchronously prepares annotation, group and point
data so their hashes can be checked too. This is **not** the application's new
asynchronous annotation schedule. No graphics API submission is measured.
The loader receives an empty progress callback; loop checks remain, but UI
progress publication is not measured.

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_OBJ_BENCHMARKS=ON
cmake --build --preset vs2026-vcpkg --config Release --target woby_mapping_adaptive woby_mapping_diagnostic woby_mapping_split1 woby_mapping_split8 woby_mapping_baseline woby_mapping_hybrid
uv run tests/vertex_mapping/contract_test.py build/vs2026-vcpkg/bin/Release
uv run tests/vertex_mapping/run.py --bin build/vs2026-vcpkg/bin/Release --models D:/temp/obj_tests --output build/mapping-detail.jsonl --rounds 3 --variants adaptive diagnostic split1 split8
```

Run measurements serially, without a concurrent build, viewer or test suite.
The runner rotates variant order each round and rejects output mismatches both
across variants and across rounds. Hashing occurs after measured work. Output
paths must be new: the runner refuses to overwrite an existing result file.
