"""Summarize independent run medians; retain failures and raw observations."""
import argparse
import json
from pathlib import Path
from statistics import median


def samples(values):
    return {"median": median(values), "min": min(values), "max": max(values), "runs": len(values)}


def reduction(before, after):
    return (1 - after / before) * 100 if before else None


def summarize(rows, after_mode="shader"):
    models = {}
    for row in rows:
        models.setdefault(Path(row["model"]).name, []).append(row)
    summary = []
    for name, model_rows in models.items():
        model_rows = [row for row in model_rows if row["mode"] in ("current", after_mode)]
        failures = [row for row in model_rows if not row["ok"]]
        modes = {mode: [row for row in model_rows if row["ok"] and row["mode"] == mode]
                 for mode in ("current", after_mode)}
        result = {"model": name, "failures": failures}
        complete_rounds = set(row["round"] for row in modes["current"]) & set(row["round"] for row in modes[after_mode])
        result["paired_rounds"] = sorted(complete_rounds)
        # Unmatched successful shader runs remain evidence of feasibility, not speedup.
        result["successful_runs"] = {mode: len(values) for mode, values in modes.items()}
        if not complete_rounds:
            summary.append(result)
            continue
        paired = {mode: [row for row in values if row["round"] in complete_rounds] for mode, values in modes.items()}
        reference = paired["current"][0]
        for values in paired.values():
            for row in values:
                for key in ("point_entries", "point_hash", "vertices", "indices", "groups", "width", "height", "msaa", "vsync"):
                    if row[key] != reference[key]:
                        raise ValueError(f"mismatched workload: {name} {key}")
        for key in ("point_entries", "vertices", "indices", "groups", "width", "height", "msaa", "vsync"):
            result[key] = reference[key]
        for key in ("enable_cpu_ms", "enable_ready_ms", "first_draw_ready_ms", "marker_payload_bytes", "base_ready_ms"):
            result[key] = {mode: samples([row[key] for row in values]) for mode, values in paired.items()}
            result[key]["reduction_percent"] = reduction(result[key]["current"]["median"], result[key][after_mode]["median"])
        result["scenarios"] = []
        for scenario in reference["scenarios"]:
            aggregate = {"name": scenario["name"]}
            for metric in ("gpu_markers", "gpu_frame", "wall_frame", "cpu_submit"):
                aggregate[metric] = {}
                for mode, values in paired.items():
                    observations = [next(s for s in row["scenarios"] if s["name"] == scenario["name"])[metric] for row in values]
                    if any(not s["count"] for s in observations):
                        raise ValueError(f"missing timing samples: {name} {metric}")
                    aggregate[metric][mode] = samples([s["median_ms"] for s in observations])
                    aggregate[metric][mode]["sample_counts"] = [s["count"] for s in observations]
                aggregate[metric]["reduction_percent"] = reduction(aggregate[metric]["current"]["median"], aggregate[metric][after_mode]["median"])
            result["scenarios"].append(aggregate)
        summary.append(result)
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.input.read_text().splitlines()]
    result = {"summary": summarize(rows), "flat_summary": summarize(rows, "flat"), "runs": rows}
    comparisons = []
    for path in sorted(args.input.parent.glob("round-*/*/image-comparison.json")):
        comparisons.append({"run": str(path.parent.relative_to(args.input.parent)), "scenarios": json.loads(path.read_text())})
    result["image_comparisons"] = comparisons
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    for item in result["summary"]:
        print(item["model"], "paired rounds:", item["paired_rounds"])
        if not item["paired_rounds"]:
            continue
        enable = item["enable_ready_ms"]
        print(f"  enable ready: {enable['current']['median']:.2f} -> {enable['shader']['median']:.2f} ms")
        for scenario in item["scenarios"]:
            gpu = scenario["gpu_markers"]
            print(f"  {scenario['name']}: {gpu['current']['median']:.3f} -> {gpu['shader']['median']:.3f} ms ({gpu['reduction_percent']:.1f}%)")


if __name__ == "__main__":
    main()
