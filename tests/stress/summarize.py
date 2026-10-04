"""Condense retained case files without discarding the underlying raw evidence."""

import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import re
import statistics

from run import write_json


def analysis_summary(result):
    summary = {}
    for side in ("aToB", "bToA"):
        value = result.get(side)
        if not value:
            continue
        summary[side] = {key: value.get(key) for key in ("triangleCount", "sampleCount", "maximum", "mean", "percentile95", "percentAboveTolerance", "diagnostics", "surfaceMeshQuality")}
        summary[side]["detectors"] = {
            key: {field: detector[field] for field in ("status", "count", "knownCount", "knownDuplicateCount", "findingCount", "findingsTruncated", "truncated", "detectionTruncated", "truncationReason", "error", "candidateTests", "candidateLimit", "pairLimit") if field in detector}
            for key, detector in value.get("detectors", {}).items() if isinstance(detector, dict)}
    if result.get("uvQuality"):
        value = result["uvQuality"]
        summary["uvQuality"] = {k: v for k, v in value.items() if k not in ("findings", "histogram")}
        if "overlaps" in summary["uvQuality"]:
            summary["uvQuality"]["overlaps"] = {k: v for k, v in summary["uvQuality"]["overlaps"].items() if not isinstance(v, list)}
    return summary


def collect(root):
    cases = []
    for path in sorted(root.glob("*/*/case.json")):
        value = json.loads(path.read_text(encoding="utf-8"))
        if "suite" not in value:
            continue
        value["evidence"] = path.parent.relative_to(root).as_posix()
        if value.get("forced_cleanup") and value.get("exit_code") == 0xFFFFFFFF and value["status"] == "process_exit":
            value["status"] = "failed"
            value["classification_note"] = "Transport failure followed by harness cleanup; Windows termination code is not evidence of an application crash"
        elif (0xC0000000 <= value.get("exit_code", 0) < 0xE0000000
              or (value.get("exit_code", 0) > 0x80000000 and not value.get("forced_cleanup"))) and value["status"] not in ("process_private_limit", "system_available_floor"):
            value["status"] = "process_exit"
            if value.get("forced_cleanup"):
                value["classification_note"] = "Windows exception exit code recorded while harness cleanup was attempted; exit/cleanup ordering is uncertain"
        value["analysis_results"] = []
        value["rpc_errors"] = []
        value["export_results"] = []
        ops = path.parent / "operations.jsonl"
        if ops.exists():
            for line in ops.read_text(encoding="utf-8").splitlines():
                operation = json.loads(line)
                result = operation.get("response", {}).get("result", {})
                if operation.get("method") == "analysis.results" and result:
                    value["analysis_results"].append(dict(name=operation["name"], **analysis_summary(result)))
                if operation.get("response", {}).get("error"):
                    value["rpc_errors"].append(dict(name=operation["name"], **operation["response"]["error"]))
                if operation.get("method") == "analysis.export-status" and result.get("export", {}).get("state") != "running":
                    value["export_results"].append(dict(name=operation["name"], **result.get("export", {})))
        resource_path = path.parent / "resources.json"
        if resource_path.exists():
            value["phase_resources"] = resource_summary(json.loads(resource_path.read_text(encoding="utf-8")))
        log_path = path.parent / "viewer.log"
        if log_path.exists():
            log = log_path.read_text(encoding="utf-8", errors="replace")
            value["cpu_load_events"] = [dict(vertices=int(m[0]), triangles=int(m[1]), groups=int(m[2]), parse_ms=float(m[3]), total_ms=float(m[4]))
                for m in re.findall(r'perf model_cpu_load path="[^"]+" vertices=(\d+) triangles=(\d+) groups=(\d+) parse_ms=([\d.]+) total_ms=([\d.]+)', log)]
            slow = re.findall(r'perf slow_frame frame=(\d+) total_ms=([\d.]+)[^\n]+', log)
            value["slow_frame_count"] = len(slow)
            value["maximum_logged_slow_frame_ms"] = max((float(m[1]) for m in slow), default=None)
        # The first render-matrix used generic mesh modes for pure clouds. Keep
        # observations but label them before any aggregation; empty draws are not
        # evidence of fast point rendering. Corrected cloud runs have explicit names.
        if path.parent.parent.name == "render-matrix" and value.get("stats", {}).get("triangleCount") == 0:
            for frame in value["frames"]:
                if frame["name"] in ("edges", "transparent"):
                    frame["measurement_note"] = "EXCLUDE: generic mesh settings disabled all standalone points"
                elif frame["name"] == "solid":
                    frame["measurement_note"] = "Default standalone points visible; label inherited from mesh scenario"
        if any("Analysis display exceeds the supported 32-bit GPU buffer size" in error.get("message", "")
               and error.get("name", "").startswith("capture_uv_quality") for error in value["rpc_errors"]):
            for frame in value.get("frames", []):
                if frame["name"].startswith("uv_quality"):
                    frame["measurement_note"] = "EXCLUDE: UV heatmap display exceeded the GPU buffer limit; no successful quality drawing"
        if path.parent.parent.name == "native-ui":
            for frame in value.get("frames", []):
                if frame["name"] == "native_properties_visible_repeat":
                    frame["measurement_note"] = "MISLABELED: Properties remained hidden after clicking its text instead of pin. Use native_properties_restored for the visible repeat."
        cases.append(value)
    return cases


def resource_summary(samples):
    phases = defaultdict(list)
    cpu = Counter()
    previous = None
    for sample in samples:
        phases[sample["phase"]].append(sample)
        if previous and previous["phase"] == sample["phase"]:
            cpu[sample["phase"]] += max(0, sample["cpu_seconds"]-previous["cpu_seconds"])
        previous = sample
    return {name: dict(samples=len(values), peak_private_bytes=max(v["private"] for v in values),
                       last_private_bytes=values[-1]["private"], last_rss_bytes=values[-1]["rss"],
                       sampled_cpu_seconds=cpu[name])
            for name, values in phases.items()}


def collect_cpu(root):
    result = []
    for path in sorted(root.glob("*/*/case.json")):
        value = json.loads(path.read_text(encoding="utf-8"))
        if "command" not in value or "suite" in value:
            continue
        if value.get("exit_code") == 1 and value.get("status") == "process_exit":
            value["status"] = "rejected"
        value["evidence"] = path.parent.relative_to(root).as_posix()
        for name in ("stdout", "stderr"):
            source = path.parent / (name+".txt")
            if source.exists():
                value[name] = source.read_text(encoding="utf-8", errors="replace")
        result.append(value)
    return result


def aggregate(cases):
    imports = defaultdict(list)
    for case in cases:
        if case.get("load_outcome", {}).get("addedCount") == 1:
            for op in case.get("operations", []):
                if op["name"] == "model_add" and op["status"] == "ok":
                    imports[Path(case["model"]).name].append(op["seconds"])
    return {model: dict(count=len(values), min_seconds=min(values), median_seconds=statistics.median(values), max_seconds=max(values), observations=values)
            for model, values in imports.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root, output = args.input.resolve(), args.output.resolve()
    cases = collect(root)
    result = dict(source_commit="7f23b97560877266ca75fbc00c38a03e9576a656", raw_root=str(root),
                  case_counts=dict(Counter(case["status"] for case in cases)),
                  import_aggregate=aggregate(cases), cases=cases, cpu_cases=collect_cpu(root))
    for name in ("environment", "dataset-manifest", "vcpkg-baseline"):
        source = root / (name+".json")
        if source.exists():
            result[name] = json.loads(source.read_text(encoding="utf-8-sig"))
    result["run_manifests"] = {str(p.parent.relative_to(root)): json.loads(p.read_text(encoding="utf-8"))
                               for p in sorted(root.glob("*/manifest.json"))}
    output.parent.mkdir(parents=True, exist_ok=True)
    write_json(output, result)
    with output.with_suffix(".csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["evidence", "model", "suite", "status", "scenario", "fps", "sampled_gpu_median_ms", "peak_private_bytes", "note"])
        for case in cases:
            for frame in case.get("frames", []):
                writer.writerow([case["evidence"], Path(case["model"]).name, case["suite"], case["status"], frame["name"],
                                 frame.get("fps"), frame.get("sampled_gpu_median_ms"), case.get("peak_private_bytes"), frame.get("measurement_note", "")])
    print(json.dumps(dict(cases=len(cases), states=result["case_counts"], output=str(output))))


if __name__ == "__main__":
    main()
