"""Summarize paired shader audit runs without mixing camera positions or rounds."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics


def summarize(report):
    if not isinstance(report.get("exact_samples"), bool):
        raise ValueError("Audit did not finish all sample checks and timings")
    if not report.get("rounds"):
        raise ValueError("Audit has no timing evidence")
    rows = {(r["kind"], r["point_size"], r["variant"], r["round"]):r for r in report["rounds"]}
    if len(rows) != len(report["rounds"]): raise ValueError("Duplicate timing row")
    grouped = defaultdict(list)
    for key, row in rows.items():
        kind, size, variant, round_index = key
        reference = rows.get((kind, size, "reference", round_index))
        if reference is None: raise ValueError("Timing row has no paired reference")
        actual, baseline = row["gpu_ms"], reference["gpu_ms"]
        if not actual or len(actual) != len(baseline) or any(not math.isfinite(t) or t<=0 for t in actual+baseline):
            raise ValueError("Unpaired or invalid timing data")
        grouped[key[:3]].append(dict(round=round_index, milliseconds=statistics.median(actual),
            paired_ratio=statistics.median(a/b for a,b in zip(actual, baseline))))
    result = []
    for (kind, size, variant), rounds in grouped.items():
        reference_rounds = grouped[(kind, size, "reference")]
        if {r["round"] for r in rounds} != {r["round"] for r in reference_rounds}:
            raise ValueError("Variant has missing rounds")
        checks = [r for r in report["validation"] if (r["kind"],r["point_size"],r["variant"]) == (kind,size,variant)]
        if not checks: raise ValueError("Timing row has no sample comparison")
        if any(r["samples"]<=0 for r in checks): raise ValueError("Comparison has no samples")
        medians = [r["milliseconds"] for r in rounds]
        result.append(dict(kind=kind, point_size=size, variant=variant, rounds=rounds,
            median_ms=statistics.median(medians), round_range_ms=[min(medians),max(medians)],
            reduction_percent=100*(1-statistics.median(r["paired_ratio"] for r in rounds)),
            exact=all(r[k]==0 for r in checks for k in ("different_colors","different_ids","different_depths")),
            samples=sum(r["samples"] for r in checks)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    summaries = {}
    for path in args.runs:
        report = json.loads((path/"measurements.json").read_text())
        summaries[path.name] = summarize(report)
        print(path.name)
        for row in summaries[path.name]:
            print(f"{row['kind']:20} {row['point_size']:2g} {row['variant']:22} {row['median_ms']:8.3f} ms  {row['reduction_percent']:6.1f}%  exact={row['exact']}")
    if args.output: args.output.write_text(json.dumps(summaries, indent=2))


if __name__ == "__main__":
    main()
