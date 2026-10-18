# Detector performance evidence

Read the [analysis report](../../detector-performance-analysis-20261017.md) for
the test conditions, timing boundaries, findings, and measurement limits.
Open [report.html](report.html) for the interactive overview. Each PNG figure
also has an SVG version.

| File | Contents |
| --- | --- |
| `summary.json` | Capture metadata, individual results, timing scopes, and summary rows. |
| `timings.csv` | Operation medians, ranges, CPU time, memory, outcomes, and observed compiler overlap. |
| `phases.csv` | Individual timing scopes, CPU time, page faults, process I/O, and retained-data counts. |
| `hardware.csv` | Raw sampled-event counts, sampling periods, period-scaled estimates, and alignment sensitivity. |
| `hardware-summary.json`, `.csv` | Stage IPC, branch rates, cache/TLB rates, and symbol coverage. |
| `queries.json`, `.csv` | Tree visits, triangle tests, sample selection, hints, and histogram percentile intervals. |
| `predicates.json`, `.csv` | Intersection broad-phase counts and robust predicate paths. |
| `detectors.json`, `.csv` | The nine automatic detector counts. |
| `power.json`, `.csv` | System-wide frequency and package/core power, summarized by phase. |
| `application.json`, `.csv` | Headless viewer request times and process measurements. |
| `cancellation.json`, `.csv` | Actual cancellation request times and return delays. |
| `raw-evidence.zip` | Per-capture JSON, logs, available generated source, build provenance, profiler exports, and validation logs. |
| `evidence-manifest.json` | Exact byte counts and SHA-256 hashes for archive members and the archive. |

Keep native latency, profiled event estimates, and instrumented query counts
separate. A rejected difference is not successful construction. A truncated
intersection result is incomplete. Selected query batches do not produce valid
full-mesh distance statistics. Compiler-overlap flags retain noisy captures;
the report uses clean repeats for the maximum-expression timing comparison.
Structured CSV cells use JSON. Empty scalar cells mean that a value is absent.

The raw archive excludes source models, executables, PDB files, and large binary
profiler databases. Those remain in
`D:/woby-builds/detector-analysis-20261017/measurements` on the measurement host.
Capture metadata identifies the matching symbols and executable hashes.

Regeneration commands are in the
[probe README](../../../experiments/detector-profile/README.md). Run analysis,
export, plotting, builds, and tests after measured workloads have stopped.
