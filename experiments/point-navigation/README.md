# Point navigation measurements

This experiment checks the production point renderer against the original shader
at `ef2112afd50410b4189484e120ebb27c29fde956`. The performance report is
[`doc/point-navigation-performance.md`](../../doc/point-navigation-performance.md).

## Build and reference

Use the `vs2026-vcpkg` preset and Release configuration. Keep the baseline
executable, its DLLs, and its complete `assets` directory before a change. For a
separate baseline checkout on Windows, use `D:/.worktree`.

```powershell
cmake --preset vs2026-vcpkg -DWOBY_BUILD_POINT_VALIDATION=ON
cmake --build --preset vs2026-vcpkg --config Release
```

The baseline raster shader used in this investigation has SHA-256
`6a87dfa3070a99c6d46cc17a8793e4364205b093835ffbf631f30a9c3dfe4e9e`.
The reference probe retains the shader's original 80-byte root prefix. The new
coverage fields follow that prefix.

## Desktop timing

Use a new output directory for each command. Run each campaign separately. Do
not run a build, test suite, or another viewer during a campaign.

```powershell
uv run tests/point_render_benchmark.py build/vs2026-vcpkg/bin/Release/woby.exe D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj build/points-new --rounds 3 --sizes 1 4 8 --full-frames 24 --navigation-frames 72 --visible-window
```

Use `--zoom 0.35` for a close view. Use `--points-only` to disable surfaces and
edges. Use the retained baseline executable for the reference campaign.

The runner requests a 1280 x 720 drawable and records the actual viewport and
4x MSAA setting. Each round rotates the point-size order. Camera commands repeat
the same twelve views. Full-detail measurements process every source point.
Adaptive measurements use the active time budget and can show fewer points.

The runner retains every frame event. GPU samples can arrive after the CPU frame
that submitted their work. `raster_frame_gpu_ms` selects frames with completed
point work. `point_raster_ms` measures the point pass. GPU times and RPC-driven
frame intervals do not measure display latency.

The overlap and memory guards abort a run if another monitored workload starts,
process private memory exceeds 38 GiB, available memory falls below 6 GiB, or the
run exceeds 20 minutes. Model, executable, source, and deployed shader hashes are
recorded. Source hashes describe the runner's checkout. Executable and deployed
shader hashes identify a retained binary from another checkout.

PNG export uses the separate full-source quad path. These exports give scene
context. Use the sample comparison below to validate the compute renderer.

## Exact GPU comparison

```powershell
uv run experiments/point-navigation/validate.py build/vs2026-vcpkg/bin/Release/woby_point_validation.exe D:/temp/obj_tests/20190810_BearTrap_Ground_Model.obj build/point-navigation-baseline-app/assets/shaders/spirv/cs_opaque_raster.bin build/points-validation-new
```

The probe uses the production upload and point submission code. It compares
every 64-bit depth/ID pair with the original shader. It runs 24 cases: 1x and 4x
MSAA, two camera distances, two camera angles, and 1/4/8-pixel circles. It checks
52,761,600 sample pairs per model. A mismatch fails the run. Timing from this
readback probe is excluded from desktop performance results.

The wrapper uses absolute input and output paths and a unique temporary working
directory. It records the same memory and overlap limits as the timing runner.

## Archive

```powershell
uv run --no-project python experiments/point-navigation/summarize.py doc/benchmarks/points-new build/points-baseline build/points-new
```

The summary uses the median of round medians. It retains their range and the
largest round p95. Each gzip file contains the complete original measurement
record. Mixed scene conditions, duplicate rounds, failed runs, guard failures,
and nonfinite timings are rejected.

Run `ctest --test-dir build/vs2026-vcpkg -C Release --output-on-failure -j 4`
for the full suite. The point GPU tests cover dense overlap, equal-depth IDs,
clipped and empty queries, viewport edges, refinement, backend changes, colors,
and 1/4x MSAA. The reporting tests check round aggregation and rejected evidence.
