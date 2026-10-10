# Shared source position implementation

Item 2 now shares an immutable coordinate array between `MeshGeometry` and `SourceMeshData`. Source metadata can be removed while render snapshots retain their coordinates. The coordinate owner does not retain source indices or importer identities.

OBJ construction records the source position ID when it creates a render vertex. An identity prefix requires no mapping. The first nonidentity ID changes the representation to a four-byte map per render vertex. Importer construction transfers the double coordinate array to the shared owner after localization and preserves importer point identities. Freeform construction appends one coordinate and retains the separate render and source offsets.

Transformed comparison meshes and UV layouts retain direct coordinates. Generated meshes can still use float positions. `meshPosition` handles all three representations. Publishing geometry validates position counts and mapped IDs. Publishing an unchanged mesh does no scan.

Rebasing copies the coordinate array once, including unused source positions. Geometry and source metadata use the new owner; prior snapshots retain the old owner. Arbitrary position edits use `editMeshPositions`, which materializes direct coordinates and clears source analysis metadata. Callers must update float positions and finalize the mesh after these edits.

The logical saving is 20 bytes per mapped render vertex, or 24 bytes with identity indexing. GPU vertex storage does not change. Actual capacity savings depend on vector growth. A detached analysis snapshot can retain unused source coordinates; this can be larger than a direct render-position array if the source has many unused points.

## Validation

Tests cover identity and mapped storage, OBJ seams, distinct equal source positions, unused positions, mixed triangle/line/point input, importer precision, freeform offsets, copies, rebasing, metadata removal, direct coordinate edits, and invalid mappings. Existing loader equivalence checks now compare resolved coordinates.

## Benchmark method

The pre-change Release executable and its DLLs were saved in `build/position-storage-baseline` before the first production build. The production executable uses the actual loader and position accessor. Both execute the same bounds, dimensions, group setup, and comparison preparation workloads. The runner compares all input geometry/source fingerprints and all operation output fingerprints. Timings exclude validation hashes, destructors, and memory sampling. Each operation has one warmup.

The historical prototype measurements remain in [their own report](source-position-model-performance.md). They are not production results. No timing here measures detector execution, GPU frame time, or interactive FPS.

## Reproduction

Configure with the `vs2026-vcpkg` preset and `WOBY_BUILD_BENCHMARKS=ON`, then build the `woby_position_production` target in Release mode. Run:

```powershell
./experiments/position-access/run.ps1 `
  -Model 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj' `
  -BaselineDirectory build/position-storage-baseline `
  -OutputDirectory build/position-production-bus -Runs 3 -Samples 3
```

The baseline directory must contain the pre-change `woby_position_before.exe` and its runtime DLLs. It is a local ignored build artifact; the runner cannot reconstruct it from the current production target. Raw records include binary and model hashes. Use one run and one sample for BearTrap to limit repeated scans of its 75.8 million triangles.

## Production results, 10 October 2026

All input fingerprints, stored source IDs, and output fingerprints matched. BusGameMap uses three fresh processes per variant and three measured samples per operation, plus one warmup. The table gives the median of process medians. BearTrap uses one fresh process per variant and one measured sample after warmup; its small timing differences are not evidence of a stable speedup or regression. No builds or tests ran during the measurements.

| Model | Operation | Before (ms) | Production (ms) | Change |
| --- | --- | ---: | ---: | ---: |
| bus | comparison_world | 78.568 | 77.847 | -0.92% |
| bus | dimensions_uncached | 84.585 | 86.413 | +2.16% |
| bus | group_bounds | 75.159 | 74.941 | -0.29% |
| bus | group_setup | 96.465 | 96.009 | -0.47% |
| beartrap | comparison_world | 6022.331 | 6074.039 | +0.86% |
| beartrap | dimensions_uncached | 5023.113 | 5086.776 | +1.27% |
| beartrap | group_bounds | 5591.816 | 5601.758 | +0.18% |
| beartrap | group_setup | 7077.531 | 7157.712 | +1.13% |

| Model | Retained input measure | Before (MiB) | Production (MiB) | Reduction (MiB) |
| --- | --- | ---: | ---: | ---: |
| bus | position_storage_capacity_bytes | 18.40 | 3.07 | 15.33 |
| bus | steady_input_private_bytes | 82.62 | 67.18 | 15.44 |
| beartrap | position_storage_capacity_bytes | 1300.87 | 216.81 | 1084.06 |
| beartrap | steady_input_private_bytes | 5651.25 | 4565.07 | 1086.17 |

Position capacity counts the old direct coordinate array or the production mapping array. The shared source coordinates are required in both variants and are excluded from this incremental figure. Unlike the earlier prototype, the production maps keep normal loader growth capacity. Render vertex capacities match between variants. Process private bytes also include allocator state and other input buffers; they are not peak loading memory.

The completed implementation did not reproduce the isolated 2x random-read penalty in these application workloads. This supports the memory optimization for these two models. It does not establish results for highly shuffled seams, detector execution, point/line picking, or GPU frame time. Loader duration is recorded as setup information only; file caches were not controlled.

The full Debug build and Release benchmark build passed without warnings. All 108 CTest entries passed in 453.13 seconds. After the final provenance-preservation and benchmark-validation edits, the full Debug build passed again and all 9 affected test entries passed in 28.34 seconds. This includes freeform GPU/render checks and coordinate-origin checks. See the [complete raw production results](source-position-production-results.json) for hashes, samples, and process memory counters.
