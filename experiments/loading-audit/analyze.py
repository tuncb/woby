"""Reduce captured intervals without adding parent and worker durations."""
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics
import sys

PROFILE = Path(__file__).resolve().parents[1] / "obj-profile"
sys.path.insert(0, str(PROFILE))
from e2e_analyze import distribution, first_marks, phases, summarize_native, timechart_series
from e2e_evidence import vtune_rows, nsys_summary


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def union_ms(intervals):
    merged = []
    for start, end in sorted(intervals):
        if end < start:
            raise ValueError("Reversed interval")
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return sum(end - start for start, end in merged)


def summarize_groups(records, fields):
    grouped = defaultdict(list)
    for row in records:
        grouped[row["configuration"]].append(row)
    return {key: {field: distribution([row[field] for row in rows]) for field in fields} for key, rows in grouped.items()}


def origin(record):
    metadata = next(row for row in record["trace"] if row["kind"] == "metadata")
    offset = (metadata["steady_epoch_nanoseconds"] - record["observer_epoch_qpc_ns"]) / 1e6
    return first_marks(record["trace"])["model_add_requested"] + offset


def series(record):
    zero = origin(record)
    result = {"cpu": [], "memory": [], "gpu": [], "system": [], "threads": []}
    samples = record["samples"]
    for a, b in zip(samples, samples[1:]):
        span = b["time_ms"] - a["time_ms"]
        result["cpu"].append({"time_s": ((a["time_ms"] + b["time_ms"]) / 2 - zero) / 1000,
                              "logical_cpus": (b["cpu_ms"] - a["cpu_ms"]) / span})
        before = {row["id"]: row["cpu_ms"] for row in a["thread_cpu"]}
        result["threads"].append({"time_s": (b["time_ms"] - zero) / 1000,
            "usage": {str(row["id"]): max(0, (row["cpu_ms"] - before[row["id"]]) / span)
                      for row in b["thread_cpu"] if row["id"] in before}})
    result["memory"] = [{"time_s": (s["time_ms"] - zero) / 1000,
        **{k: s[k] for k in ("working_set", "private_bytes", "system_available_memory")}} for s in samples]
    result["gpu"] = [{"time_s": (s["time_ms"] - zero) / 1000, **{k: v for k, v in s.items() if k != "time_ms"}}
                     for s in record.get("gpu", {}).get("samples", [])]
    result["system"] = [{"time_s": (s["time_ms"] - zero) / 1000, "values": s["values"]}
                        for s in record.get("system_samples", [])]
    return result


def engine_values(values, pid, kind):
    if not isinstance(values, list):
        return []
    return [v["value"] for v in values if f"pid_{pid}_" in v["instance"] and f"engtype_{kind}" in v["instance"]]


def resources_by_phase(record):
    zero = origin(record)
    phase_zero = first_marks(record["trace"])["model_add_requested"]
    result = []
    for phase in phases(record):
        if phase["wall_ms"] < 40 or phase["name"].endswith("_worker"):
            continue
        begin, end = phase["start_ms"] - phase_zero, phase["end_ms"] - phase_zero
        gpu = [s for s in record.get("gpu", {}).get("samples", []) if begin <= s["time_ms"] - zero < end]
        system = [s["values"] for s in record.get("system_samples", []) if begin <= s["time_ms"] - zero < end]
        result.append({"name": phase["name"], "start_ms": begin, "end_ms": end,
            "wall_ms": phase["wall_ms"], "cpu_ms": phase["cpu_ms"], "caller_cpu_ms": phase["caller_cpu_ms"],
            "logical_cpus": phase["average_logical_cpus"], "caller_utilization": phase["caller_cpu_ms"] / phase["wall_ms"],
            "machine_cpu_percent": phase["average_logical_cpus"] / 16 * 100,
            "commit_begin": phase["commit_begin"], "commit_end": phase["commit_end"],
            "working_set_begin": phase["working_set_begin"], "working_set_end": phase["working_set_end"],
            "read_bytes": phase["read_bytes"], "page_faults": phase["page_faults"], "data": phase["data"],
            "device_gpu_percent": distribution([s["gpu_percent"] for s in gpu]),
            "process_3d_percent": distribution([v for s in system for v in engine_values(s.get("gpu_engines"), record["pid"], "3D")]),
            "process_copy_percent": distribution([v for s in system for v in engine_values(s.get("gpu_engines"), record["pid"], "Copy")]),
            "system_disk_read_Bps": distribution([s["disk_read_bytes_per_second"] for s in system
                                                    if isinstance(s.get("disk_read_bytes_per_second"), (int, float))])})
    return result


def summarize(root):
    native = summarize_native(root)
    endpoint_rows = []
    traced = []
    for item in read(root / "native/manifest.json")["runs"]:
        record = read(Path(item["output"]) / "result.json")
        marks = first_marks(record["trace"])
        row = {"configuration": item["configuration"], "round": item["round"], "ready_rpc_ms": record["load_rpc_ms"]}
        if "model_add_requested" in marks:
            row.update({key: marks[key] - marks["model_add_requested"] for key in
                        ("first_scene_frame_gpu_complete", "all_tools_ready", "ready_toast_drawn", "ready_toast_frame_gpu_complete")})
            if item["configuration"] == "traced-visible":
                traced.append((item, record, row))
        endpoint_rows.append(row)
    median_toast = statistics.median(row[2]["ready_toast_drawn"] for row in traced)
    item, record, endpoints = min(traced, key=lambda row: abs(row[2]["ready_toast_drawn"] - median_toast))
    phase_rows = resources_by_phase(record)
    zero = first_marks(record["trace"])["model_add_requested"]
    parser = read(root / "parser/results.json")
    parser_summary = summarize_groups(parser["records"], ("total_ms", "scan_ms", "merge_ms", "attribute_allocate_ms", "merge_execute_ms",
        "average_logical_cpus", "parse_cpu_ms", "peak_working_set_after_parse", "private_bytes_after_parse", "input_prepare_ms",
        "worker_input_ms_sum", "worker_decode_ms_sum", "workers", "chunks", "retained_input_bytes"))
    controls = read(root / "controls/results.json")
    control_rows = []
    for row in controls:
        phase = next(p for p in row["events"] if p["name"] == ("memory_copy" if row["mode"] == "memory" else "read_only"))
        control_rows.append({"configuration": row["mode"] + "-w" + str(row["workers_requested"]), "round": row["round"],
            "wall_ms": phase["wall_ms"], "logical_cpus": phase["average_logical_cpus"],
            "GBps": row["stats"].get("payload_GB_per_second", row["stats"].get("input_bytes", 0) / phase["wall_ms"] / 1e6)})
    hardware = {case: read(root / "deep" / case / "cpu-analysis.json") for case in ("branch", "cache", "fp")}
    result = {"provenance": read(root / "provenance.json"), "cpu": controls[0]["cpu"], "native": native,
        "endpoint_rows": endpoint_rows, "representative": {"name": Path(item["output"]).name, "endpoints_ms": endpoints,
            "phase_zero_ms": zero, "phase_rows": phase_rows, "phases": phases(record), "series": series(record),
            "parser_workers": [t for t in record["trace"] if t["kind"] == "worker"], "stats": record["stats"],
            "process_peak_private_bytes": max(s["peak_commit"] for s in record["samples"]),
            "process_peak_working_set": max(s["peak_working_set"] for s in record["samples"]),
            "screenshot": str(Path(item["output"]) / "beartrap.png"), "pid": record["pid"]},
        "parser": parser_summary, "parser_records": parser["records"], "controls": summarize_groups(control_rows, ("wall_ms", "GBps", "logical_cpus")),
        "hardware": {case: {k: v for k, v in data.items() if k in ("stages", "event_definitions", "time_alignment_note",
            "instruction_sample_categories", "clock_alignment_ms")} for case, data in hardware.items()},
        "vtune_hotspots": vtune_rows(root / "deep/vtune/hotspots-function.csv", "Function")[:30],
        "gpu_trace": nsys_summary(root / "deep/nsys-wddm-retry"),
        "validation": {"parser_fingerprints": sorted({r["fingerprint"] for r in parser["records"]}),
                       "native_runs": len(endpoint_rows), "parser_runs": len(parser["records"]), "control_runs": len(controls)}}
    for values in result["hardware"].values():
        values["time_alignment_note"] = "QPC metadata alignment. The saved probe includes a +16 ms sensitivity check; this is not a new measurement of an exact residual offset."
    timechart = root / "deep/timechart/timechart"
    csv_path = next(timechart.rglob("*.csv"), None)
    if csv_path:
        power_record = read(root / "deep/timechart/result.json")
        result["power"] = timechart_series(csv_path, power_record)
        power_zero = origin(power_record)
        for sample in result["power"]:
            sample["time_s"] = (sample["time_ms"] - power_zero) / 1000
            sample["max_lp_frequency_MHz"] = max(value for key, value in sample["values"].items() if "effective-frequency" in key)
        result["phase_power"] = []
        for phase in phases(power_record):
            if phase["wall_ms"] < 100 or phase["name"].endswith("_worker"):
                continue
            trace_offset = power_zero - first_marks(power_record["trace"])["model_add_requested"]
            selected = [s for s in result["power"] if phase["start_ms"] + trace_offset <= s["time_ms"] < phase["end_ms"] + trace_offset]
            result["phase_power"].append({"name": phase["name"], "samples": len(selected),
                "max_lp_frequency_MHz": distribution([s["max_lp_frequency_MHz"] for s in selected]),
                "mean_lp_frequency_MHz": distribution([s["mean_effective_frequency_MHz"] for s in selected]),
                "package_power_W": distribution([s["package_power_W"] for s in selected])})
    if (root / "census.json").exists():
        result["census"] = read(root / "census.json")
    calibration = next((root / "calibration").glob("*fp-analysis.json"), None)
    if calibration:
        result["calibration"] = read(calibration)
    kernel_path = root / "kernels.json"
    if kernel_path.exists():
        result["kernels"] = read(kernel_path)
        groups = defaultdict(list)
        for row in result["kernels"]["records"]:
            groups[row["name"]].append(row["wall_ms"])
        result["kernel_summary"] = {key: distribution(values) for key, values in groups.items()}
    simd_path = root / "parser-simd/analysis.json"
    if simd_path.exists():
        result["parser_simd"] = read(simd_path)
    micro_path = root / "parser-microarchitecture/analysis.json"
    if micro_path.exists():
        result["parser_microarchitecture"] = read(micro_path)
    return result


if __name__ == "__main__":
    root = Path(sys.argv[1]).resolve(strict=True)
    output = Path(sys.argv[2]).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(summarize(root), indent=2), encoding="utf-8")
    print("Saved current loading summary: " + str(output))
