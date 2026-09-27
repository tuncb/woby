# Accelerated hover-picking experiment

Research prototypes for the five models in `D:/temp/obj_tests`. Production source
in the main checkout is unchanged. `instrument.py` installs the prototypes in a
disposable worktree at the recorded source commit. Without the experiment config
environment variable, the viewer uses its normal renderer selection, mouse input,
hover picker, and shutdown path.

## Variants

- `none`: draw the scene without hover queries.
- `legacy`: the production linear vertex-hover scan.
- `cpu`: a median-split point BVH, with up to 256 points per leaf. A conservative
  cursor frustum rejects nodes; surviving points use the production radius,
  clipping, depth, distance, and original-point-order rules. There is no
  front-to-back depth pruning in this prototype.
- `gpu_cluster`: one 256-thread workgroup per spatial leaf. One lane culls the
  cluster against the cursor frustum. Surviving lanes project candidate points;
  three compute passes reduce candidates to a single winning identity.
- `gpu_flat`: the same shader, buffers, and reductions with cluster culling
  disabled. It scans the spatially reordered point list, not original-order data.
- `gpu_immediate`: clustered compute with readback requested in the same API
  frame as the copy, to expose D3D11 synchronization cost.
- `gpu_resident`: clustered compute without a copy or readback; a diagnostic
  control that cannot supply CPU coordinate text.

Both GPU variants reuse the renderer's positions and original point IDs. They add
a reordered rank list, cluster bounds, group transforms, reduction buffers, and
eight tiny readback textures. Requests use a bounded ring; the application does
not wait for results. The pinned bgfx D3D11 backend uses a blocking `Map` on its
render thread. The default GPU variants therefore defer the read request until
three application frame advances after issuing the copy. This trades result age
for a reduced chance of blocking the render thread. It is not a guarantee that
the driver cannot wait. The prototype logs completed results without displaying stale coordinate
text. CPU results use the ordinary coordinate overlay. No GPU-highlight UI is
implemented. No model is modified.

These variants implement geometric vertex proximity, including points behind
solid surfaces. They do **not** implement visible-fragment picking. The GPU's
floating-point projection can choose a different winner when projected depths
are nearly equal; exact agreement with the CPU is recorded rather than assumed.

The experiment supports one static model per process, its existing scene groups,
and changing camera/display settings. The index is built once. Production mesh
replacement, background construction, cancellation, and repeated instances would
need lifecycle integration before shipping. Equal-depth/equal-distance ties use
the original imported group/point order; user-reordered scene nodes need an
explicit traversal-order key before production integration. No editable scene property is added.

## Measurement

The normal multithreaded full viewer, bgfx renderer, ImGui, and presentation run
in a hidden 1280×720 window on NVIDIA D3D11, four-sample MSAA, VSync off. The bgfx
per-view profiler records compute-pass GPU timestamps without showing a profiler
overlay. Baselines use the same profiling setting. All modes query on every
eligible frame, including orbit/pan; this deliberately bypasses the production
drag suppression and static-result cache. Native input latency is not measured.

Every process primes the index and GPU resources before timed scenarios. Buffers
remain resident in all variants, including controls. A scenario warms for at
least 1.5 seconds and 20 frames, then measures for at least 3 seconds and 30
frames. Two core rounds shuffle scenario order and reverse model order. Extra
solid/alpha/depth/fixed-center controls have one round and should be treated as exploratory.
Fixed-center controls include both fitted and 4× farther views, with cache reuse
disabled, so the pointer sweep's frequent misses cannot hide dense-query costs.
There are no simultaneous builds, tests, or other viewers during timing.

FPS is frames/elapsed time. CPU stage timings exclude render-thread/presentation
waiting. Per-view GPU samples use each view's own source frame number, deduplicate
repeated timestamps, and exclude eight frames at each measured boundary. GPU
readback latency is issue-to-observation wall time; its final eight issued frames
are excluded to prevent a following slow legacy scenario inflating latency.
GPU and CPU times overlap. Frequencies, thermals, and scheduling are not locked.
Per-frame timing/counters are retained in memory and written at shutdown.

Validation is separate from timing: ten deterministic camera/cursor positions
per case cover a two-second motion cycle. CPU results are checked against both a
linear reference and the production picker. GPU results are compared with the
CPU reference, and clustered/unculled GPU sequences are compared exactly. These
runs execute extra reference work and must never be interpreted as performance.
The recorded validation sweep covers the nine original cases; the later
fixed-center timing controls isolate dense queries; the fitted center pose is
also covered by the zoom/orbit/pan sweeps. CPU timings include misses; hit counts and hit-only
timing distributions are retained to make easy rejection cases explicit.

## Reproduction

From the main checkout, with its dependency tree already installed:

```powershell
git worktree add --detach D:/.worktree/woby-hover-picking HEAD
uv run tests/hover_experiment/instrument.py D:/.worktree/woby-hover-picking
cd D:/.worktree/woby-hover-picking
cmake --preset vs2026-vcpkg -DVCPKG_INSTALLED_DIR=D:/woby/build/vs2026-vcpkg/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF -DWOBY_TEST_HEADLESS=ON
cmake --build --preset vs2026-vcpkg
ctest --test-dir build/vs2026-vcpkg -C Debug --output-on-failure
cmake --build --preset vs2026-vcpkg --config Release --target woby
cd D:/woby
uv run tests/hover_experiment/test_analysis.py
uv run tests/hover_experiment/run.py --binary D:/.worktree/woby-hover-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/hover-qa-new --rounds 1 --validate --suite all
uv run tests/hover_experiment/run.py --binary D:/.worktree/woby-hover-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/hover-core-new --rounds 2
uv run tests/hover_experiment/run.py --binary D:/.worktree/woby-hover-picking/build/vs2026-vcpkg/bin/Release/woby.exe --output build/hover-extra-new --rounds 1 --suite extra
uv run tests/hover_experiment/summarize.py build/hover-core-new/runs.jsonl build/hover-extra-new/runs.jsonl --output build/hover-results-new.json
```

The report artifacts can be regenerated with:

```powershell
uv run tests/hover_experiment/summarize.py build/hover-core/runs.jsonl build/hover-extra/runs.jsonl --output doc/hover-picking-results.json
uv run tests/hover_experiment/check_validation.py build/hover-qa/runs.jsonl doc/hover-picking-validation.json
uv run tests/hover_experiment/report.py
uv run --with matplotlib tests/hover_experiment/plot.py doc/hover-picking-results.json doc/hover-picking-experiment.png
```

`report.py` describes this recorded run, including its fixed hardware, case counts,
and validation totals. Update its narrative before using it for a different run.

Use fresh output directories. `--pilot --model-filter Bus --rounds 1` is a short
smoke run. Debug timings and validation timings are not comparable to Release
performance runs. The process timeout is one hour per model; failures are not
silently dropped. Unit-test files use a unique temporary root and clean up after
failures too.
