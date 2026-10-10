# Analysis display records per face

The four display paths now reuse indexed source geometry. Surface quality stores four normalized metrics and one flat normal. UV quality stores the encoded value, highlight state, and all three corner normals. Distance display stores four float distances, three CPU-rounded midpoints, and four sample normals. Comparison wireframes generate edge endpoints from the triangle buffer.

| Incremental payload per source face | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Surface quality | 96 B | 32 B | 66.7% |
| UV quality | 96 B | 48 B | 50.0% |
| Distance display vertices | 384 B | 112 B | 70.8% |
| Comparison wireframe indices | 24 B | 0 B | 100% |

These counts exclude the source vertex/index buffers, allocator overhead, raw quality measurements, and the four double distances and four double areas required for distance statistics. Quality preparation buffers are released after upload. Distance records remain in the CPU result and on the GPU. The first implementation commit (`dc0854f`) retains the 48-byte CPU identity index array. The separate cleanup below removes it.

`setFaceDisplay` binds the face record and source triangle buffer for generated corner draws. It checks the record stride, face count, and starting face. The draw retains its buffers until execution. An explicit zero vertex count now gives an empty draw. UV records use geometry face offsets independently of measurement offsets and node order.

Quality metric changes reuse one GPU buffer. UV refresh keeps the original display normals even after source normals are smoothed. Distance samples and area statistics still use the original double calculations. CPU-rounded midpoints are stored because shader float midpoint arithmetic can change geometry at large coordinates. The maximum distance record count under the 32-bit buffer byte limit rises from 11,184,810 to 38,347,922 faces. Other geometry and device limits still apply.

## Validation

The complete Debug build passed without warnings. All **110 CTest groups** passed (445.12 seconds), including 26 graphics groups. The new GPU fixture produces exactly equal color and marker-target pixels for compact and expanded quality, UV, distance, and wireframe draws. It covers all four quality metrics, invalid/unavailable values, distinct UV corner normals, depth overlap, clipping, alpha-blended lines, MSAA on/off, offsets above 16 bits, empty draws, invalid bindings, and buffer release after submission.

CPU tests cover double midpoint rounding at large coordinates, per-sample normals and areas, degenerate faces, UV measurement offsets and reordered nodes, cancellation, stage publication, GPU buffer reuse, and source-edit invalidation. Existing app smoke tests cover UV views, transparency, picking, and loading. Slang generated Metal source for both changed vertex entry points without warnings. Metal GPU execution and Apple metallib compilation were not available on this Windows host.

## BusGameMap performance

The model has 1,054,542 faces and 547,469 render vertices. Measurements used Release probes on an AMD Ryzen 7 5800H machine with an NVIDIA GeForce RTX 3070 Laptop GPU. The [raw results and hashes](face-display-storage-results.json) include every timing sample. These are representation comparisons in one executable, not full application before/after frame rates.

For CPU packing, three fresh processes each ran one warmup and seven samples in alternating order. Every output vertex and metric was checked against the expanded reference. The table uses the median of the three process medians.

| CPU work | Expanded (ms) | Compact (ms) | Change |
| --- | ---: | ---: | ---: |
| Quality packing | 109.798 | 62.957 | -42.66% |
| Sequential checksum reads | 46.041 | 45.369 | -1.46% |
| Shuffled checksum reads | 112.418 | 194.495 | +73.01% |

Checksum reads are a CPU locality stress test. Production drawing uses the GPU. The shuffled CPU penalty is not a measured slowdown in picking, dimensions, or UI operations.

GPU measurements used a 1024 × 768 offscreen target without MSAA. Each of four alternating blocks ran 12 warmup frames and 31 samples. The warmup excludes the renderer's three-frame timestamp delay and pipeline creation. The table uses the median of block medians. Timings include the clear and one draw; analysis, upload, and readback are excluded. There were no concurrent tests or benchmarks.

| Geometry | GPU draw | Expanded (ms) | Compact (ms) | Change |
| --- | --- | ---: | ---: | ---: |
| Indexed | quality | 0.3082 | 0.3084 | +0.04% |
| Indexed | uv | 0.2652 | 0.2616 | -1.35% |
| Indexed | distance | 1.0276 | 1.0380 | +1.01% |
| Indexed | wireframe | 0.5571 | 0.5499 | -1.29% |
| Unshared | quality | 0.2847 | 0.3683 | +29.36% |
| Unshared | uv | 0.2836 | 0.4086 | +44.04% |
| Unshared | distance | 1.0461 | 1.0487 | +0.24% |
| Unshared | wireframe | 0.6043 | 0.5997 | -0.76% |

The imported indexed case stays within about 1.4% in this run; such small differences do not establish a speed gain. Removing source vertex sharing makes quality and UV fetches slower by about 0.084 ms and 0.125 ms per draw. Keep this trade-off visible: smaller buffers do not guarantee faster shaders. Wireframe block timings vary more than quality timings. These measurements do not cover all camera views, GPU vendors, or large-model interactive sessions.

For this model, each quality buffer saves 64.36 MiB, each UV quality buffer saves 48.27 MiB, each distance display buffer saves 273.55 MiB, and each comparison wireframe saves 24.14 MiB. CPU and GPU savings have different lifetimes as described above. BearTrap was not loaded for this display experiment; its capacity figures in the investigation remain estimates.

## Reproduction

```powershell
$env:VCPKG_ROOT = 'C:\work\vcpkg'
$env:VULKAN_SDK = 'C:\VulkanSDK\1.4.357.0'
cmake --build --preset vs2026-vcpkg --config Debug
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure -j 4
cmake --build --preset vs2026-vcpkg --config Release --target woby_face_display_probe woby_face_display_gpu_probe
./experiments/face-display/run.ps1 -Model 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj' -OutputDirectory build/face-display/production-results
./experiments/face-display/run_gpu.ps1 -Model 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj'
```

The GPU probe uses the shaders staged by the Debug build and a Release CPU executable. Shader optimization is controlled by the shader build. It compares both layouts in the same production shader, with the expanded reference path retained for equivalence tests. The indexed fixture uses the imported model vertex sharing. The unshared fixture gives each triangle corner its own source vertex. Both have the same faces and positions. Positions are normalized to the viewport; display values are fixed. Analysis and upload are outside GPU timings.

## Comparison picking cleanup

The second change removes `SurfaceComparison::sampledIndices`, its allocation, its identity-index writes, and its stage transfer. No production reader used this array. Picking already reads `surface.source`; dimensions use the same source pick parts. The picking algorithm stays the same.

This saves **48 CPU bytes per source face**, or **48.27 MiB per side** on BusGameMap. Across both changes, retained CPU distance display data falls from 432 bytes per face (expanded vertices and indices) to 112 bytes, a 74.1% reduction. The 64 bytes of double distances and sample areas remain separate and unchanged. GPU payload does not change in this second commit because the identity indices were never uploaded.

A new regression test computes both comparison sides, removes all display records, then checks source-based picking, source triangle identity, translated dimensions, selection outlines, and tolerance statistics. It also checks that source vertex/index storage and the measurements stay unchanged. This protects the source-query contract without requiring a sampled mesh.

The cleanup passed the complete Debug build without warnings and all 76 unit test groups (57.20 seconds), including eight GPU unit groups and the large-mesh checks. The first change's performance measurements still apply to rendering; the cleanup changes CPU distance-result storage only. Probe metadata records the parent HEAD before the first commit; executable and source hashes identify the tested implementation.
