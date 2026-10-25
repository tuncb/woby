# Shader audit evidence

See [the report](../../shader-performance.md) for the change, measurements,
rejected candidates, and limits.

- `manifest.json` lists 32 complete campaigns, two excluded attempts, 40
  candidate/control builds, six application runs, and file hashes.
- `summary.csv` contains the paired GPU summaries. Positive reduction means
  less GPU time. A reduction is the median of frame-paired ratios, not the ratio
  of the two displayed time medians. Compare each candidate with its unchanged
  `repeat` control in the same campaign.
- Each `*-run.json.gz` contains the configuration, input and shader hashes,
  executable hash, source hashes, resource use, and exit status.
- Each `*-measurements.json.gz` retains raw GPU frame times and sample mismatch
  counts. Completed probe runs require `exact_samples`. The interrupted
  Powerplant attempts are excluded from `summary.csv`.
- Each `*-summary.json.gz` contains the paired summary and round ranges.
- `candidates.zip` contains candidate sources, reference sources, compiler
  commands, validation logs, binary hashes, two intermediate probe snapshots,
  final probe sources, and evidence scripts. It does not contain shader or
  application binaries. Earlier screens have source hashes in their run records;
  not every intermediate probe source was saved.
- `application-summary.json` compares full-detail, adaptive, and cached
  application frames. The longer navigation check reverses application order.
  Its sparse-workload variation does not support a separate speed claim.
- `application-exports.zip` stores the nine pairs of matching exported images,
  deduplicated by PNG hash. `images.json` inside the archive maps each hash to
  its original campaign and filename.
- `verification.json` records all 44 production shader hashes and Metal
  translation commands. Only three compute entries differ from the reference.
  Metal translation is not native Metal compilation or a Metal GPU test.
- `normalization.json` records matching normalized SPIR-V hashes for three
  source forms of the sample specialization.
- `machine.json` records the tested hardware and tool versions.

The logs include the Release build, complete CTest run, camera-test rechecks,
final probe tests, and Vulkan core/synchronization validation. The full suite
initially passed 133 of 134 entries. The camera CLI test then passed three
consecutive reruns without a code change. Preserve the original failure when
reporting these results.

Read compressed JSON with Python's standard library:

```python
import gzip, json
with gzip.open("semantic100m-generic-measurements.json.gz", "rt") as stream:
    report = json.load(stream)
```

The large input models stay in their original local directories. Their paths
and SHA-256 hashes are in the run records. GPU clocks and temperature were not
fixed. The timing results apply to the tested NVIDIA Vulkan system.
