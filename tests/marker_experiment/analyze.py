"""Summarize source-frame-attributed timing and completed marker requests."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def distribution(values):
    values = sorted(values)
    if not values:
        return dict(n=0, median=None, p95=None)
    index = (len(values)-1)*.95
    lo = int(index)
    p95 = values[lo] + (values[min(lo+1,len(values)-1)]-values[lo])*(index-lo)
    return dict(n=len(values), median=statistics.median(values), p95=p95)


def view_times(events, view, low, high):
    key, stamp = f"view_{view}", f"view_frame_{view}"
    by_frame = {e[stamp]: e[key] for e in events if key in e and stamp in e and low <= e[stamp] <= high and e[key] >= 0}
    return distribution(list(by_frame.values()))


def eligible_completions(completions, scenario, last_frame):
    return [c for c in completions if c["scenario"] == scenario and c["measured"] and c["frame"] < last_frame-8]


def summarize(paths):
    runs = []
    for path in paths:
        for line in path.read_text().splitlines():
            run = json.loads(line)
            marker_path = path.resolve().parent / Path(run["marker_path"]).name
            data = json.loads(marker_path.read_text())
            run["marker_path"] = str(marker_path)
            csv_path = marker_path.with_name(marker_path.name.removesuffix(".marker.json") + ".csv")
            with csv_path.open() as stream:
                frames = [{k: float(v) for k,v in row.items()} for row in csv.DictReader(stream)]
            for i,row in enumerate(run["scenarios"]):
                measured = [f["frame"] for f in frames if f["scenario"] == i and f["measured"]]
                low,high = min(measured)+8,max(measured)-8
                row["views"] = {str(v): view_times(data["events"],v,low,high) for v in (1,2,3,4,6)}
                completed = eligible_completions(data["completions"],i,max(measured))
                row["readback_ms"] = distribution([c["latency_ms"] for c in completed])
                row["readback_frames"] = distribution([c["latency_frames"] for c in completed])
                row["hits"] = sum(c["id"] != 0 for c in completed)
                row["requests"] = len(completed)
                row["submit_ms"] = distribution([e["cpu_ms"] for e in data["events"] if e["kind"]=="submit" and e["scenario"]==i and e["measured"]])
            run["marker_metadata"] = data["metadata"]
            runs.append(run)
    return runs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("runs", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    runs = summarize(args.runs)
    args.output.write_text(json.dumps(runs,indent=2))
    with args.output.with_suffix(".csv").open("w",newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["model","round","scenario","fps","p95_frame_ms","scene_gpu_ms","lookup_gpu_ms","composite_gpu_ms","highlight_gpu_ms","readback_ms","readback_frames","hits","requests"])
        for run in runs:
            for row in run["scenarios"]:
                if row["name"]=="prime": continue
                writer.writerow([run["model"],run["round"],row["name"],row["fps"],row["wall_p95_ms"],
                                 *(row["views"][str(v)]["median"] for v in (1,2,3,4)),
                                 row["readback_ms"]["median"],row["readback_frames"]["median"],row["hits"],row["requests"]])
    print(f"Summarized {len(runs)} runs, {sum(len(r['scenarios'])-1 for r in runs)} measured scenarios excluding priming")


if __name__=="__main__":
    main()
