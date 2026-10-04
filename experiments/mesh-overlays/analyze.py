# /// script
# requires-python = ">=3.11"
# dependencies = ["Pillow>=11"]
# ///
"""Aggregate recorded runs and check image parity for unchanged display modes."""
import argparse
import csv
import json
from pathlib import Path
import statistics

from PIL import Image, ImageChops


def compare(left, right):
    with Image.open(left) as a, Image.open(right) as b:
        if a.size != b.size:
            raise ValueError(f"Capture dimensions differ: {a.size} versus {b.size}")
        difference = ImageChops.difference(a.convert("RGB"), b.convert("RGB"))
        maximum = ImageChops.lighter(ImageChops.lighter(*difference.split()[:2]), difference.split()[2])
        counts = maximum.histogram()
        return {"different_pixels": sum(counts[1:]), "over_8_levels": sum(counts[9:]),
                "max_channel_difference": max(i for i, count in enumerate(counts) if count)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("runs", nargs="+", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Aggregate output must be a new directory")
    args.output.mkdir(parents=True)
    aggregates, checks, inputs = [], [], []
    for run in args.runs:
        guard = json.loads((run / "guard.json").read_text())
        if guard["status"] != "completed" or guard["exit_code"] != 0:
            raise ValueError(f"Incomplete experiment: {run}")
        directory = run / "captures"
        report = json.loads((directory / "results.json").read_text())
        inputs.append(dict(path=str(run.resolve()), guard=guard,
                           metadata={k: v for k, v in report.items() if k != "results"}))
        groups = {}
        for result in report["results"]:
            groups.setdefault((result["scenario"], result["method"]), []).append(result)
            scenario, method, round_id = result["scenario"], result["method"], result["round"]
            reference = "legacy" if scenario in {"solid", "vertices_1", "vertices_4", "vertices_8"} else "barycentric"
            if method == reference:
                continue
            reference_path = directory / f"{scenario}-{reference}-r{round_id}.png"
            if not reference_path.exists():
                continue
            measured_path = directory / f"{scenario}-{method}-r{round_id}.png"
            check = dict(run=run.name, scenario=scenario, method=method, reference=reference, round=round_id,
                         **compare(measured_path, reference_path))
            checks.append(check)
            if reference == "legacy" and check["different_pixels"] != 0:
                raise ValueError(f"Unchanged display mode lost image parity: {check}")
        for (scenario, method), results in groups.items():
            times = [row["gpu_ms"] for row in results]
            aggregates.append(dict(run=run.name, model=Path(report["model"]).name, scenario=scenario,
                                   method=method, rounds=len(results), samples=report["samples"],
                                   ids=report["id_attachment"], width=report["width"], height=report["height"],
                                   gpu_ms=statistics.median(times), gpu_min_ms=min(times), gpu_max_ms=max(times),
                                   cpu_ms=statistics.median(row["cpu_submit_ms"] for row in results),
                                   non_background_pixels=results[0]["non_background_pixels"]))
    payload = dict(inputs=inputs, aggregates=aggregates, image_checks=checks)
    (args.output / "results.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
    with (args.output / "results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(aggregates[0]))
        writer.writeheader()
        writer.writerows(aggregates)
    print(f"{len(aggregates)} aggregates; {len(checks)} image comparisons; unchanged modes are pixel-identical")


if __name__ == "__main__":
    main()
