"""Summarize recorded endpoints and phases without adding nested intervals."""
import csv
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re
import statistics


def percentile(values, fraction):
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    position = (len(values) - 1) * fraction
    low = int(position); high = min(low + 1, len(values) - 1)
    return values[low] + (values[high] - values[low]) * (position - low)


def distribution(values):
    values = [v for v in values if v is not None]
    if not values:
        return {"count": 0}
    return {"count": len(values), "median": statistics.median(values), "min": min(values), "max": max(values),
            "p95": percentile(values, .95), "p99": percentile(values, .99)}


def frame_summary(capture):
    events = capture.get("events", [])
    result = {"frames": len(events), "dropped_frames": capture.get("droppedFrames", 0),
              "cpu_frame_ms": distribution([e["frameMilliseconds"] for e in events]),
              "gpu_frame_ms": distribution([e.get("gpuFrameMilliseconds") for e in events]),
              "environments": capture.get("environments", []),
              "point_submission_total": sum(e.get("points", {}).get("submittedCount", 0) for e in events)}
    if len(events) >= 2:
        span = events[-1]["completedMilliseconds"] - events[0]["completedMilliseconds"]
        result["frames_per_second"] = (len(events) - 1) * 1000 / span if span > 0 else None
    result["stages"] = {name: distribution([e["stagesMilliseconds"].get(name) for e in events])
                        for name in (events[0].get("stagesMilliseconds", {}) if events else [])}
    result["points"] = {name: distribution([e.get("points", {}).get(name) for e in events])
                        for name in ("sourceCount", "submittedCount", "gpuRasterCount", "gpuRasterMilliseconds")}
    return result


def first_marks(trace):
    result = {}
    for record in trace:
        if record["kind"] == "mark":
            result.setdefault(record["name"], record["time_ms"])
    return result


def phases(record):
    return [t for t in record.get("trace", []) if t["kind"] == "phase"]


def log_values(path):
    text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
    result = {}
    cpu = re.search(r'perf model_cpu_load path=.*?parse_ms=([\d.]+) total_ms=([\d.]+)', text)
    upload = re.search(r'perf model_gpu_finalize path=.*?bytes=(\d+) upload_ms=([\d.]+) submissions=(\d+) waits=(\d+).*?staging_bytes=(\d+)', text)
    if cpu:
        result.update(cpu_import_ms=float(cpu[1]), initial_cpu_preparation_ms=float(cpu[2]))
    if upload:
        result.update(initial_upload_bytes=int(upload[1]), initial_upload_ms=float(upload[2]),
                      initial_upload_submissions=int(upload[3]), initial_upload_waits=int(upload[4]), staging_bytes=int(upload[5]))
    return result


def native_first_frame_bounds(record):
    capture = record.get("load_frames", {})
    initial_revision = capture.get("environments", [{}])[0].get("sceneEditRevision", 0)
    event = next((e for e in capture.get("events", [])
                  if capture["environments"][e["environment"]]["sceneEditRevision"] != initial_revision), None)
    begin = next((r for r in record.get("rpc", []) if r["method"] == "performance.begin"), None)
    if not event or not begin:
        return None
    # This is CPU frame completion, bracketed by the begin RPC. It is not a
    # GPU fence or a physical display timestamp.
    return [begin[key] + event["completedMilliseconds"] - record["load_request_ms"] for key in ("start_ms", "end_ms")]


def summarize_run(path):
    record = json.loads(path.read_text(encoding="utf-8"))
    result = {"name": path.parent.name, "path": str(path), "hidden": record["hidden"],
              "profiler": record.get("profiler"), "error": record.get("error"), "executable_sha256": record["executable_sha256"]}
    if result["error"]:
        return result
    result.update({k: record.get(k) for k in ("load_rpc_ms", "load_to_verified_screenshot_ms", "startup_ms")})
    result.update(log_values(path.parent / "viewer.log"))
    marks = first_marks(record.get("trace", []))
    if "model_add_requested" in marks:
        result["milestones_ms"] = {k: value - marks["model_add_requested"] for k, value in marks.items()}
        if "first_scene_frame_gpu_complete" in marks and "all_tools_ready" in marks:
            result["after_display_preparation_ms"] = marks["all_tools_ready"] - marks["first_scene_frame_gpu_complete"]
    result["native_scene_frame_cpu_complete_bounds_ms"] = native_first_frame_bounds(record)
    result["render"] = frame_summary(record.get("frames", {}))
    result["load_render"] = frame_summary(record.get("load_frames", {}))
    result["phase_times_ms"] = {}
    for phase in phases(record):
        result["phase_times_ms"].setdefault(phase["name"], []).append(phase["wall_ms"])
    samples = record.get("samples", [])
    result["process"] = {field: distribution([s[field] for s in samples if field in s])
                         for field in ("working_set", "private_bytes", "peak_working_set", "peak_commit", "threads")}
    result["observer_interval_ms"] = distribution([b["time_ms"] - a["time_ms"] for a, b in zip(samples, samples[1:])])
    begin = record.get("render_started_ms", 0); end = begin + record.get("render_duration_ms", 0)
    gpu = [s for s in record.get("gpu", {}).get("samples", []) if begin <= s["time_ms"] < end]
    result["settled_gpu"] = {field: distribution([s.get(field) for s in gpu])
                              for field in ("gpu_percent", "memory_used", "sm_clock_MHz", "memory_clock_MHz", "temperature_C", "power_W", "pcie_rx_KB_per_second", "pcie_tx_KB_per_second", "pcie_link_generation", "pcie_link_width")}
    result["gpu_clock_event_reasons"] = sorted({s.get("clock_event_reasons", 0) for s in gpu})
    result["memory_maps"] = record.get("memory_maps", [])
    result["feature_captures"] = [{"name": c["name"], "start_ms": c["start_ms"], "end_ms": c["end_ms"],
                                   "render": frame_summary(c["frames"]), "performance": c["performance"],
                                   "transition": frame_summary(c.get("transition_frames", {})), "image": c.get("image")}
                                  for c in record.get("feature_captures", [])]
    result["navigation"] = frame_summary(record.get("navigation_frames", {}))
    return result


def summarize_native(root):
    manifest = json.loads((root / "native/manifest.json").read_text())
    runs, groups = [], {}
    for item in manifest["runs"]:
        result = summarize_run(Path(item["output"]) / "result.json")
        result.update(round=item["round"], configuration=item["configuration"])
        runs.append(result); groups.setdefault(item["configuration"], []).append(result)
    statistics_by_group = {}
    for name, rows in groups.items():
        statistics_by_group[name] = {field: distribution([r.get(field) for r in rows])
                                    for field in ("load_rpc_ms", "load_to_verified_screenshot_ms", "cpu_import_ms", "initial_cpu_preparation_ms", "initial_upload_ms", "after_display_preparation_ms")}
        statistics_by_group[name]["first_frame_gpu_ms"] = distribution([r.get("milestones_ms", {}).get("first_scene_frame_gpu_complete") for r in rows])
        statistics_by_group[name]["phases_ms"] = {phase: distribution([sum(r["phase_times_ms"].get(phase, [])) for r in rows])
                                                  for phase in sorted({p for r in rows for p in r["phase_times_ms"]})}
    return {"runs": runs, "groups": statistics_by_group}


def timechart_series(path, viewer):
    text = path.read_text(encoding="utf-8-sig")
    lines = text.splitlines(); start = next(i for i, line in enumerate(lines) if line.startswith("RecordId,Timestamp,"))
    date = datetime.strptime(next(line.split(",")[1] for line in lines if line.startswith("Profile Start Time:")), "%b-%d-%Y_%H-%M-%S").date()
    epoch = datetime.fromisoformat(viewer["observer_epoch_utc"])
    offset = timezone(timedelta(seconds=viewer["timezone_offset_seconds"]))
    result = []
    for row in csv.DictReader(lines[start:]):
        if not row.get("Timestamp"):
            continue
        hour, minute, second, millisecond = map(int, row["Timestamp"].split(":"))
        timestamp = datetime(date.year, date.month, date.day, hour, minute, second, millisecond * 1000, tzinfo=offset)
        values = {key: float(value) for key, value in row.items() if key not in ("RecordId", "Timestamp") and value and value.strip()}
        frequencies = [value for key, value in values.items() if "effective-frequency" in key]
        result.append({"time_ms": (timestamp - epoch).total_seconds() * 1000, "values": values,
                       "mean_effective_frequency_MHz": statistics.mean(frequencies),
                       "package_power_W": values.get("socket0-package-power")})
    return result
