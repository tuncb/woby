# Marker-ID GPU hover experiment

Research implementation for `D:/temp/obj_tests`, installed into a disposable
worktree. Main-checkout production code is unchanged. This extends the earlier
`hover_experiment` and uses the full viewer measurement adapter in `render_fps`.

The [recorded report](../../doc/marker-picking-experiment.md) includes 270 measured
scenarios across all five models, plus the earlier clustered-compute reference.
Opaque fitted-view FPS falls by about 2–16%; at marker opacity 0.4, the loss is
41–44%. GPU highlighting avoids the coordinate readback wait, but not normal
rendering/display latency. This is a research prototype, not production integration.

## Rendering and selection

The regular marker draw writes its usual color plus a second RGBA8 attachment.
The attachment contains the original point-list rank plus one, encoded as four
exact bytes. IDs use a flat, full-register varying. An independent blend state preserves IDs when marker color
is transparent. Surface fragments write an empty ID, including translucent
surfaces: an underlying marker's blended color contribution does not retain its
ID when such a surface is drawn over it. Fully transparent markers
are discarded. No spatial sorting, BVH, cluster construction, or additional
geometry pass is used.

Four-sample MSAA is retained. The ID attachment is sampled as a multisample
texture, **never averaged/resolved**. A compute invocation examines a 7x7 pixel
neighborhood and each sample. It selects the nearest covered pixel center within
three pixels of the cursor, breaking ties by point ID. Depth testing in the scene
pass determines which samples are visible. This is visible-fragment selection, not the previous geometric center
radius/frontmost-vertex policy. At partial transparency the last depth-passing
marker fragment supplies the ID; a unique perceptual owner of blended pixels is
not defined. Selection tolerance includes a few pixels around visible coverage.

An offscreen color target is resolved normally and composited into the viewport.
A small cursor-local quad reads the winning ID and tints pixels in proportion to
the number of samples matching the selected marker. Lookup and highlight execute in the same GPU frame. Helpers and
ImGui follow. No CPU result is used to draw the highlight.

Pass IDs are monotonically ordered: scene 1, lookup 2, composite 3, highlight 4,
helpers 5, readback 6. This avoids a pinned-bgfx interaction between remapped blit
sort keys and original view IDs, which initially copied the previous result.

## Asynchronous coordinates

Eight slots hold 16-byte decoded result copies and the source draws' transform snapshots.
The normal asynchronous variant defers `bgfx::readTexture` by three application
frame advances, as in the preceding experiment. bgfx supplies a future completion
frame. The CPU locates the original point ID in the captured draw ranges (linear
in draw count, not point count), reconstructs coordinates with that saved transform,
and displays them in the existing coordinate panel with an
asynchronous label. Results from previous scenarios or older requests cannot
replace newer text. Misses clear the text when their readback arrives.

The application never waits for coordinates. The pinned D3D11 bgfx implementation
still uses a blocking Map internally; this experiment does not add fence-driven
native readback. Coordinate age is measured separately from GPU lookup time and
FPS. Mouse-to-display latency is not measured.

## Controls

- `none`: original scene rendering with no picking.
- `routed`: offscreen color/depth and composite, without ID output.
- `id_buffer`: marker ID output, without cursor lookup, highlight, or readback.
- `id_resident`: ID output, lookup, and GPU highlight, without coordinate readback.
- `id_async`: GPU highlight plus delayed asynchronous coordinate text.
- `id_immediate`: GPU highlight plus immediate readback request, to expose the
  synchronization/throughput tradeoff.

`--wide` retains the initial RGBA32F format (ID limbs, depth, draw ordinal), with
depth and draw-order tie breakers. Its BearTrap pilot showed that writing the
wide attachment alone reduced fitted-view throughput from 9.5 to 5.4 FPS. The
compact format removes the per-sample depth/draw metadata. Repeated instances of
the same point range are outside this prototype's scope.

Resource creation is primed before timed scenarios. Marker resources remain
resident in controls. CPU draw-transform snapshots scale with draw count, not
point count. The RGBA8 ID target scales with viewport pixels and sample count:
13.4 MiB for the actual 1280×687 scene viewport with four samples.

## Validation

Four C++ tests cover ID encoding beyond float's exact integer range, MSAA sample
selection, viewport/tolerance clipping, and stale completion rejection. Four
Python tests check timing attribution and readback filtering. The GPU fixture
checks known overlapping vertices, solid occlusion, MSAA, alpha, zero opacity,
hidden groups, size, resizing, viewport offsets, and translating models while
coordinates are in flight. Fixtures use one absolute temporary root and are
cleaned up even on failure.

During separate model validation runs, the shader writes the entire sampled
neighborhood to an audit texture. CPU code independently reduces those samples
and compares exact results, checks encoded IDs, and validates their source draw
ranges. Audit readbacks are disabled in timing runs. Synthetic captures verify an
early GPU highlight and a later coordinate panel. Validation timings are not
performance measurements.

## Reproduction

From the main checkout:

```powershell
git worktree add --detach D:/.worktree/woby-marker-picking HEAD
uv run tests/marker_experiment/instrument.py D:/.worktree/woby-marker-picking
cd D:/.worktree/woby-marker-picking
cmake --preset vs2026-vcpkg -DVCPKG_INSTALLED_DIR=D:/woby/build/vs2026-vcpkg/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure
cmake --build --preset vs2026-vcpkg --config Release --target woby
cd D:/woby
uv run tests/marker_experiment/test_analysis.py
uv run tests/marker_experiment/validate_gpu.py --binary D:/.worktree/woby-marker-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/marker-fixture-new
uv run tests/marker_experiment/check_render.py --binary D:/.worktree/woby-marker-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/marker-colors-new
uv run tests/marker_experiment/run.py --binary D:/.worktree/woby-marker-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/marker-qa-new --rounds 1 --validate --suite all
uv run tests/marker_experiment/run.py --binary D:/.worktree/woby-marker-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/marker-core-new --rounds 2
uv run tests/marker_experiment/run.py --binary D:/.worktree/woby-marker-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/marker-extra-new --rounds 1 --suite extra
uv run tests/hover_experiment/run.py --binary D:/.worktree/woby-marker-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/marker-cluster-new --rounds 1 --scenario-filter fit_gpu_cluster
uv run tests/hover_experiment/summarize.py build/marker-cluster-new/runs.jsonl --output doc/marker-cluster-reference.json
uv run tests/marker_experiment/analyze.py build/marker-core-new/runs.jsonl build/marker-extra-new/runs.jsonl --output doc/marker-picking-results.json
uv run --with matplotlib tests/marker_experiment/plot.py doc/marker-picking-results.json doc/marker-picking-experiment.png
uv run tests/marker_experiment/report.py
```

Use fresh output directories. Run models serially with no other viewers, builds,
or tests active. Hidden 1280x720 window, NVIDIA D3D11, VSync off; the actual scene
viewport excludes the menu bar. Each scenario warms for at least 1.5 seconds and
20 frames, then measures at least 3 seconds and 30 frames. Two core rounds reverse
model order and shuffle scenarios. Extra controls have one round. Per-view GPU
timestamps are deduplicated by their own source frame with eight-frame margins;
readback summaries exclude the last eight issue frames. Clocks and thermals are
not locked. Source/binary provenance and raw captures accompany the report.

The experiment supports one static imported model, including its scene groups.
The fixture exercises changing transforms; replacing models, cancellation, repeated
instances, native pointer handling, and integration with other scene renderers
need production lifecycle work. No new persisted scene property is introduced.
