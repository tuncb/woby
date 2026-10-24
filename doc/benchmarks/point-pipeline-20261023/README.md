# Point pipeline evidence — 23 October 2026

See [the investigation report](../../point-pipeline-performance.md) for methods,
results, scope, and decisions.

`summary.json` contains round medians, ranges, model and binary hashes, archive
hashes, and exact-sample totals. It separates shader probes from desktop
application runs. It uses the median of independent round p50 values. The probe
uses the upper middle order statistic for each even-sized round.

- `beartrap-fitted` and `beartrap-close`: six-method prototype comparisons.
- `confirm-fitted` and `confirm-close`: reference and retained production shaders,
  alternating their order at each identical camera frame.
- `confirm-single`: one-round BearTrap check at 1x MSAA.
- `confirm-bennu` and `confirm-100m`: paired checks on another mesh and a point-only
  model. The latter has no surface depth seed.
- `desktop-reference`, `desktop-after`, and their `-close` variants: complete
  application captures, with full-detail, adaptive, refinement, and cached phases.

Each `*-measurements.json.gz` preserves the complete measurement JSON. Probe
`*-run.json.gz` files retain commands, resource guard results, and source,
executable, model, and shader hashes. Desktop measurement files contain their
own provenance and guards. All campaigns completed without a guard violation.
Open these files with any gzip reader; no custom format is required.

The reference executable was retained before changes from commit
`d86e15d1d0e774e08ff0f255dce0148d3d3033ef`. Its SHA-256 is
`a56953c59d95b221bcb1e8da323e7c54f9c7bcbae1b466af20a09c294a8d3285`.
It already includes the first navigation optimization. Current source hashes
beside a retained reference executable describe the recording checkout; use
the executable and runtime shader hashes to identify the code that ran.

The main runs compare 432,645,120 final MSAA sample records across 132 cases.
Each record contains RGBA8 color, original uint32 picking ID, and D32_FLOAT depth
bits. All fields match. This count includes reference repeatability controls,
but excludes the small fixtures and pilot. Exported PNGs are not used as
correctness evidence because image export has a different marker path.

GPU times exclude capture and readback. The probes do not measure UI,
presentation, display FPS, or input latency. Desktop camera motion differs from
probe motion. GPU clock and temperature were not fixed. The paired runs reduce
time drift; separate desktop runs and partially rotated prototypes retain it.

`verification.json` records the warning-free Release build, 132 passing CTest
tests, three passing follow-up suites after the fixture correction, Vulkan core
and synchronization validation, and nine successful Slang Metal source outputs.
The `validation` directory contains the corresponding raw logs. Loader messages
about explicit layer selection are expected; the final runs contain no Vulkan
validation warning or error. Native Apple compilation and runtime execution
were not available on this Windows host.
