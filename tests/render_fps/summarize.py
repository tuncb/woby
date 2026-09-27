"""Aggregate independent run summaries, preserving ranges and sample counts."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def aggregate(runs):
    result = {}
    for run in runs:
        model = run["model"]
        metadata = run["metadata"]
        identity = {k: metadata[k] for k in ("models", "renderer", "vendor_id", "device_id")}
        if model not in result:
            result[model] = dict(identity=identity, scenarios={})
        if result[model]["identity"] != identity:
            raise ValueError(f"Mismatched model or renderer: {model}")
        for row in run["scenarios"]:
            entries = result[model]["scenarios"].setdefault(row["name"], [])
            if entries and entries[0]["settings"] != row["settings"]:
                raise ValueError(f"Mismatched settings: {model}/{row['name']}")
            entries.append(row)
    for model in result.values():
        for name, rows in model["scenarios"].copy().items():
            summary = dict(runs=len(rows), settings=rows[0]["settings"],
                           frames=sum(row["frames"] for row in rows),
                           gpu_samples=sum(row["gpu_samples"] for row in rows))
            for key in ("fps", "wall_p50_ms", "wall_p95_ms", "wall_p99_ms", "cpu_p50_ms", "cpu_p95_ms",
                        "gpu_p50_ms", "gpu_p95_ms", "draws", "width", "height"):
                values = [row[key] for row in rows if row[key] is not None]
                summary[key] = statistics.median(values) if values else None
                summary[key+"_range"] = [min(values), max(values)] if values else None
            summary["stages"] = {key: statistics.median(row["stages"][key] for row in rows) for key in rows[0]["stages"]}
            summary["observations"] = rows
            model["scenarios"][name] = summary
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("runs", type=Path, nargs="+")
    p.add_argument("output", type=Path)
    args = p.parse_args()
    runs = [json.loads(line) for path in args.runs for line in path.read_text(encoding="utf-8").splitlines() if line]
    summary = aggregate(runs)
    args.output.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    fields = "model scenario runs frames fps fps_min fps_max cpu_ms gpu_ms p95_ms draws hover_ms".split()
    with args.output.with_suffix(".csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for name, model in summary.items():
            for scenario, s in model["scenarios"].items():
                writer.writerow(dict(model=name, scenario=scenario, runs=s["runs"], frames=s["frames"],
                                     fps=s["fps"], fps_min=s["fps_range"][0], fps_max=s["fps_range"][1],
                                     cpu_ms=s["cpu_p50_ms"], gpu_ms=s["gpu_p50_ms"], p95_ms=s["wall_p95_ms"],
                                     draws=s["draws"], hover_ms=s["stages"]["hover_pick"]))


if __name__ == "__main__":
    main()
