# Store analysis display values per face

This report records the investigation before implementation. See [the implementation and validation report](face-display-storage-implementation.md) for the current code and GPU measurements.

Start with surface quality. A 32-byte face record can replace 96 bytes of expanded vertices while preserving all four display values and the current flat normal. The CPU prototype reconstructed every output vertex bit for bit on BusGameMap. Packing was faster, but shuffled CPU reads were slower. GPU performance and rendered pixels remain untested.

This investigation uses implementation commit `42be09d` on 10 October 2026. It adds an isolated experiment, not a production renderer change. The [raw measurements](face-display-results.json), [CPU probe](../experiments/face-display/main.cpp), and [compile-only shader](../experiments/face-display/quality_faces.slang) are included.

## What the current code retains

| Path | Current display payload per source face | Lifetime and behavior |
| --- | ---: | --- |
| Surface quality | 3 × 32 = 96 bytes | Worker builds expanded vertices. All four normalized metrics fit in the first two corners' UV fields. Flat normal and positions are repeated. CPU array is freed after successful upload; GPU array remains. |
| UV quality | 3 × 32 = 96 bytes | Each corner copies a source vertex and receives the same encoded value and highlight flag. Prepared CPU buffer is released after upload. |
| Distance | 12 × 32 = 384 bytes, plus 12 × 4 = 48 CPU index bytes | Four explicit subtriangles per source face. CPU sampled vertices and indices remain in the result. Only vertices are uploaded. Four double distances and four double areas add 64 bytes per face and remain necessary for statistics. |
| Comparison wireframe | 6 × 4 = 24 index bytes | GPU edge indices duplicate relationships already available from the triangle buffer. Prepared CPU copy is temporary. |

Sources: [quality packing](../src/surface_mesh_quality.cpp), [UV packing](../src/uv_quality.cpp), [distance construction](../src/mesh_comparison.cpp), [upload](../src/comparison_gpu.cpp), [post-upload release](../src/comparison_runtime.cpp).

Two corrections to the original proposal matter:

- Quality expansion is not a permanent second CPU geometry array. The expected saving is in GPU storage and CPU preparation/upload memory. Do not count both as steady CPU savings.
- [Comparison picking](../src/scene_pick.cpp) already uses `surface.source`, with a comment that sampled triangles cover the same surface. Dimensions consume those pick parts. Repository search found no production consumer of `sampled.indices` outside its construction, and upload reads only `sampled.vertices`. The sampled identity indices are a separate deletion candidate. Tests currently assert their presence, so update the result contract and its tests together. The sampled bounds, node, and origin also need a use audit if `sampled` becomes a display-buffer struct.

## Surface quality candidate

```cpp
struct FaceDisplay {
    std::array<float, 4> values;
    std::array<float, 3> normal;
    uint32_t reserved;
}; // 32 bytes
```

The normal is computed with the same `calculateFaceNormal` call as today. Values use the same double normalization and float conversion, including `-1` for degenerate faces and `-2` for unavailable values. Storing the normal avoids a shader arithmetic change and repeated normal calculations. A 16-byte values-only record would need to derive normals on the GPU; it has a different validation and performance cost. The 28-byte packed alternative also needs a separate alignment/cache experiment.

Reuse the indexed source GPU vertices and triangle indices. Draw three generated corners per face without an indexed draw. For each corner, fetch its source vertex through the triangle buffer and its metric and normal from the face record. The prototype preserves the current comparison shader's normal treatment, which differs from the ordinary mesh shader.

The application already uses this corner-fetch pattern for `vs_mesh_edges` and `vs_triangle_lines`. `SV_VertexID` avoids dependence on fragment primitive ID. A nonzero triangle offset must affect both the source-corner lookup and the face-record lookup. Binding a sliced vertex buffer currently rebases its pointer; the new path must instead pass an explicit source-corner offset.

The prototype uses existing `root.pointIds` for triangle indices and `root.freeform` for face data. Production should give this binding a clear API and lifetime contract. Use `setVertexCount(3 * faceCount)`, bind the triangle buffer as storage, and do not bind it as the draw index buffer. Preserve marker shader selection, depth, MSAA, transparency, clipping, and selected-side behavior.

Sources: [comparison shader](../shaders/native/woby.slang), [root layout](../shaders/native/root.h), [graphics bindings and draw dispatch](../src/graphics.cpp), [comparison draw calls](../src/comparison_view.cpp).

## BusGameMap measurement

The model has 1,054,542 faces and 547,469 render vertices. The probe loads the actual OBJ and computes production quality measurements before timing. It compares the production `surfaceQualityDisplayVertices` function with compact packing. It does not time loading or metric analysis.

Three fresh Release processes each perform one untimed warmup and seven measured samples. The variant order alternates within each process. Both representations remain resident. Validation, destruction of prior buffers, and output serialization are outside packing timings. Every reconstructed vertex and all four encoded metrics are checked after each packing pair. CPU read checks visit all faces either in order or in a deterministic shuffle and return matching checksums.

| Work | Expanded (ms) | Compact (ms) | Change |
| --- | ---: | ---: | ---: |
| Pack display data | 111.588 | 68.946 | -38.21% |
| Sequential CPU checksum reads | 46.320 | 45.328 | -2.14% |
| Shuffled CPU checksum reads | 113.865 | 195.910 | +72.05% |

Values are medians of the three process medians. The display payload is **101,236,032 bytes (96.55 MiB)** before and **33,745,344 bytes (32.18 MiB)** after: a **66.7% reduction**, or **64.36 MiB** per quality display buffer. Source vertices, triangle indices, raw quality results, distributions, and allocator overhead are excluded from this incremental payload comparison.

The read benchmark includes a serial checksum, and the indexed candidate adds source-index and vertex fetches. It is a CPU locality test, not a GPU simulator. Its shuffled regression is evidence to measure GPU fetch costs; its sequential result is not a frame-rate prediction. Packing gets less memory to write, while indexed reads can require more separate memory accesses.

## Capacity implications

For one million faces, 32-byte records save 61.04 MiB per quality buffer. For BearTrap's 75,771,986 faces, the arithmetic changes from **6,937.13 MiB** to **2,312.38 MiB**, a **4,624.76 MiB** reduction per side. These BearTrap display sizes are estimates, not allocations or timings from this experiment.

The existing quality-stage precheck uses `3 * faces * sizeof(Vertex)` and a 32-bit byte limit. It rejects more than **44,739,242 faces**, which includes BearTrap. A 32-byte record raises that buffer-specific limit to **134,217,727 faces** if the precheck is updated. Source buffers, total GPU memory, allocation limits, draw counts, and analysis memory still apply. This does not prove that full BearTrap analysis will fit or run acceptably.

Sources: `comparisonBufferBytes`, `validateComparisonDistanceSize`, and the quality-stage precheck in [mesh comparison](../src/mesh_comparison.cpp).

## UV quality and distance need separate contracts

UV quality can use an eight-byte value/highlight record if the renderer preserves its normal rules. [Preparation](../src/comparison_scene.cpp) intentionally packs quality before smoothing the indexed source. World comparison vertices begin with zero normals; UV layout vertices use `-Y`. Quality-only refresh explicitly restores these normals. Fetching the smoothed GPU vertex normal would change the current display. A per-draw normal can preserve the prepared world/layout paths; the generic upload path must still support its existing input normals.

Build UV records in displayed node/face order. `node.uvQualityOffset` selects the original measurement; `node.indexOffset` selects display geometry. They cannot be treated as interchangeable after layout filtering or separation. Preserve missing/collapsed precedence, orientation, overlap and threshold flags, linked selection, and refresh invalidation. Raw `UvQuality` measurements and UV layout geometry are separate storage issues.

Distance display has a precision problem that a 16-byte distance-only record does not solve. The current implementation calculates midpoints and subtriangle normals in double precision, then casts each output value to float. GPU subdivision of existing float source positions can differ. For example, endpoints 16,777,216.9 and 16,777,218.9 produce a stored midpoint of 16,777,218, but averaging their rounded float positions yields 16,777,216. Such values can occur in meshes with a large local extent.

A conservative distance record could keep the three pre-rounded float midpoints (36 bytes), four existing subtriangle normals (48 bytes), and four float distances (16 bytes): 100 bytes, or 112 with explicit padding. This is a candidate layout, not an implemented or benchmarked format. It preserves the current computations while reusing the three source corner positions. A smaller format needs evidence that precision and normal changes are acceptable. Keep double distances and areas for CPU statistics.

Deleting the 48-byte identity index array is independent of GPU subdivision. It would save 48.27 MiB for BusGameMap, or 3,468.57 MiB for BearTrap by arithmetic. BearTrap exceeds the current distance display limit, so that larger saving is not a measured current allocation.

Comparison wireframes can reuse the existing procedural triangle-line shader to remove 24 bytes of edge indices per face. Update wireframe submission and resource-validity checks together; preserve line coverage and x-ray/depth behavior. This is also independent of quality records.

## Recommended implementation and validation

1. Implement the 32-byte surface-quality record first. Change worker output, stage application, GPU upload/destruction, bindings, draw count, and the quality byte-limit check together. Keep the current raw metric arrays, distributions, result revisions, cancellation, and upload cleanup.
2. Check all metrics, sentinel colors, flat normals, winding, nonzero draw offsets, selected sides, marker IDs, clipping, MSAA, transparency, empty inputs, stale jobs, and failed uploads. Compare images and GPU timings on Vulkan and Metal. Test both low-reuse and high-reuse geometry.
3. Remove sampled identity indices and duplicate comparison edge indices as separate changes with their existing consumer contracts checked.
4. Add UV records after formalizing the display-normal and face-order contracts. Investigate distance records separately with large-coordinate and near-degenerate fixtures.

The full Debug build and Release probe build completed without warnings. The new contract test passed; it also runs in every Release probe process. It covers invalid face counts, cancellation, clamping, unavailable/degenerate sentinels, offset fetches, and full reconstruction. The prototype shader compiled with warnings as errors to SPIR-V and passed Vulkan 1.4 validation. Slang generated Metal source. No Apple `metallib` build, GPU execution, screenshot comparison, or GPU timing was performed for this candidate.

## Reproduce

Enable `WOBY_BUILD_BENCHMARKS` with the `vs2026-vcpkg` preset, then:

```powershell
cmake --build --preset vs2026-vcpkg --config Release --target woby_face_display_probe
./experiments/face-display/run.ps1 -Model 'D:\temp\obj_tests\uploads_files_2720101_BusGameMap.obj'
./experiments/face-display/compile_shader.ps1
```

The [runner](../experiments/face-display/run.ps1) records model, executable, and source hashes. [Shader compilation](../experiments/face-display/compile_shader.ps1) uses the tools and dependency paths from the configured build. These commands now use the production face packer. The original measurements above used the equivalent isolated packer; they remain the investigation baseline.
