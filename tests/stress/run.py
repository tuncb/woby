# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil", "pillow"]
# ///
"""Serial, opt-in large-file measurements of the unmodified Woby viewer.

Requires psutil (uv run --with psutil tests/stress/run.py ...). No timing assertions.
Every case gets a fresh process, an isolated working directory on the output drive,
raw RPC evidence, process memory samples, and a log. Input models are never edited.
"""

import argparse
import errno
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid

import psutil


GIB = 1024 ** 3


def fixture_copy(source, target):
    """Hard-link immutable inputs when possible; do not assume matching drives."""
    try:
        os.link(source, target)
    except OSError as error:
        if error.errno != errno.EXDEV and getattr(error, "winerror", None) != 17:
            raise
        shutil.copyfile(source, target)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False), encoding="utf-8")


def append_json(path, value):
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, allow_nan=False) + "\n")


def summarize_frames(samples):
    unique = []
    for sample in samples:
        if not unique or sample["frameIndex"] != unique[-1]["frameIndex"]:
            unique.append(sample)
    if len(unique) < 2:
        return {"samples": len(unique), "fps": None}
    elapsed = unique[-1]["midpoint"] - unique[0]["midpoint"]
    frames = unique[-1]["frameIndex"] - unique[0]["frameIndex"]
    gpu = [s["gpuFrameMilliseconds"] for s in unique if s.get("gpuFrameMilliseconds") is not None]
    return dict(samples=len(unique), elapsed_seconds=elapsed, frames=frames,
                fps=frames / elapsed if elapsed > 0 else None,
                sampled_frame_median_ms=statistics.median(s["frameMilliseconds"] for s in unique),
                sampled_frame_max_ms=max(s["frameMilliseconds"] for s in unique),
                sampled_gpu_median_ms=statistics.median(gpu) if gpu else None,
                sampled_stage_median_ms={k: statistics.median(s["stagesMilliseconds"][k] for s in unique)
                                         for k in unique[0]["stagesMilliseconds"]})


def summarize_capture(record, width, height, camera_motion=False):
    """Only complete, comparable desktop frame windows may publish FPS/tails."""
    reasons = []
    events = record.get("events", [])
    environments = record.get("environments", [])
    if record.get("droppedFrames", 0):
        reasons.append("frame_capture_overflow")
    if len(events) < 2:
        reasons.append("insufficient_frames")
    if len(environments) != 1:
        reasons.append("environment_or_scene_changed")
    for env in environments:
        drawable, viewport = env["drawable"], env["viewport"]
        if drawable != {"width": width, "height": height}:
            reasons.append("drawable_mismatch")
        if not (viewport["width"] > 0 and viewport["height"] > 0 and viewport["x"] >= 0 and viewport["y"] >= 0
                and viewport["x"] + viewport["width"] <= width and viewport["y"] + viewport["height"] <= height):
            reasons.append("invalid_viewport")
        if not env["window"]["visible"] or env["window"]["minimized"]:
            reasons.append("hidden_or_minimized")
        presentation = env["presentation"]
        if not presentation["submitted"] or (presentation["width"], presentation["height"]) != (width, height):
            reasons.append("presentation_mismatch")
    if events and any(not 0 <= e["environment"] < len(environments) for e in events):
        reasons.append("missing_environment")
    intervals = [b["completedMilliseconds"] - a["completedMilliseconds"] for a, b in zip(events, events[1:])]
    if any(b["frameIndex"] != a["frameIndex"] + 1 for a, b in zip(events, events[1:])):
        reasons.append("missing_frame_events")
    if any(not math.isfinite(v) or v <= 0 for v in intervals):
        reasons.append("invalid_frame_clock")
    if events and not camera_motion and any(e["camera"] != events[0]["camera"] for e in events):
        reasons.append("camera_changed")
    before, after = record["sceneBefore"], record["sceneAfter"]
    if {k: v for k, v in before.items() if k != "camera"} != {k: v for k, v in after.items() if k != "camera"}:
        reasons.append("scene_settings_changed")
    stats = before["stats"]
    if not stats.get("visibleGroupCount", 0) or not any(stats.get(k, 0) for k in ("triangleCount", "pointCount", "lineSegmentCount")):
        reasons.append("empty_geometry_control")
    summary = dict(valid=not reasons, exclusions=sorted(set(reasons)), fps=None, frames=len(events),
                   environment=environments[0] if len(environments) == 1 else None,
                   camera_motion=camera_motion, metric="completed_frame_intervals")
    if reasons:
        return summary
    # Nearest-rank percentiles over ALL completion intervals, never sparse GPU samples.
    ordered = sorted(intervals)
    elapsed = sum(intervals) / 1000
    summary.update(fps=len(intervals) / elapsed, elapsed_seconds=elapsed,
                   frame_interval_median_ms=statistics.median(intervals),
                   frame_interval_p95_ms=ordered[math.ceil(.95 * len(ordered)) - 1],
                   frame_interval_p99_ms=ordered[math.ceil(.99 * len(ordered)) - 1],
                   frame_interval_max_ms=max(intervals))
    return summary


def comparison_key(record, camera_motion=False):
    """Ignore session identities, retain every measured rendering condition."""
    identities = {}
    def identify(nodes):
        for node in nodes:
            if isinstance(node.get("id"), str) and node["id"] not in identities:
                identities[node["id"]] = f"object:{len(identities)}"
            identify(node.get("children", []))
    identify(record["sceneBefore"]["tree"])
    def stable(value):
        if isinstance(value, dict):
            ignored = {"sceneEditRevision", "sceneGeneration"}
            return {k: stable(v) for k, v in value.items()
                    if k not in ignored}
        if isinstance(value, list):
            return [stable(v) for v in value]
        if isinstance(value, str):
            return identities.get(value, value)
        return value
    settings = stable(dict(scene=record["sceneBefore"], environments=record["environments"], camera_motion=camera_motion,
                           runtime=record.get("runtime"), host=record.get("host")))
    return hashlib.sha256(json.dumps(settings, sort_keys=True, allow_nan=False).encode()).hexdigest()


def verify_capture(path):
    """Decode the complete PNG and reject blank output, outside timing windows."""
    from PIL import Image
    try:
        with Image.open(path) as image:
            if image.format != "PNG":
                return False
            image.verify()
        with Image.open(path) as image:
            image.load()
            return image.width > 0 and image.height > 0 and any(low != high for low, high in image.convert("RGB").getextrema())
    except (OSError, ValueError, SyntaxError):
        return False


def memory_stop_reason(private_bytes, available_bytes, cap_bytes, floor_bytes):
    if cap_bytes and private_bytes > cap_bytes:
        return "process_private_limit"
    if floor_bytes and available_bytes < floor_bytes:
        return "system_available_floor"
    return None


def memory_limit(value):
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise argparse.ArgumentTypeError("Memory limits must be finite and nonnegative; 0 disables the limit")
    return value


def add_memory_limit_arguments(parser):
    parser.add_argument("--max-private-gib", type=memory_limit, default=0,
                        help="Optional process-memory cutoff; 0 (default) disables it")
    parser.add_argument("--min-available-gib", type=memory_limit, default=0,
                        help="Optional available-memory floor; 0 (default) disables it")


def monitor(state):
    process = psutil.Process(state["process"].pid)
    while not state["stop"].wait(.2):
        try:
            memory = process.memory_info()
            cpu = process.cpu_times()
            available = psutil.virtual_memory().available
            sample = dict(t=time.perf_counter() - state["started"], phase=state["phase"],
                          rss=memory.rss, private=getattr(memory, "private", memory.vms),
                          peak_rss=getattr(memory, "peak_wset", memory.rss),
                          cpu_seconds=cpu.user + cpu.system, available=available,
                          threads=process.num_threads())
            state["resources"].append(sample)
            reason = memory_stop_reason(sample["private"], available, state["cap"], state["floor"])
            if reason:
                state["termination"] = reason
                process.kill()
                return
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            return


def rpc_request(state, method, params=None, timeout=60):
    params = dict(params or {})
    if method not in ("instance.info", "command.get"):
        params["timeoutSeconds"] = max(1, min(3600, math.ceil(timeout)))
    state["request_id"] += 1
    body = json.dumps(dict(jsonrpc="2.0", id=state["request_id"], method=method, params=params)).encode()
    req = urllib.request.Request(state["url"], data=body,
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bearer " + state["token"]})
    with urllib.request.urlopen(req, timeout=timeout + 5) as response:
        payload = json.load(response)
    return payload


def request(state, method, params=None, timeout=60):
    if timeout:
        return rpc_request(state, method, params, timeout)
    # The RPC deadline only ends the HTTP wait once a command has started.
    # Follow its ID to completion without resubmitting a load or keeping a
    # connection open indefinitely. Transport failures still fail the case.
    payload = rpc_request(state, method, params, 60)
    error = payload.get("error", {})
    command = error.get("data", {})
    if error.get("code") != -32003 or command.get("state") != "running" or not command.get("commandId"):
        return payload
    while True:
        if state["process"].poll() is not None:
            raise RuntimeError("Viewer exited while waiting for load completion")
        payload = rpc_request(state, "command.get", {"id": command["commandId"]}, 30)
        if "error" in payload:
            return payload
        result = payload["result"]
        if result["state"] not in ("queued", "running"):
            field = "error" if "error" in result else "result"
            return {field: result[field]}
        time.sleep(.2)


def add_load_timeout_argument(parser):
    parser.add_argument("--load-timeout", type=int, default=0,
                        help="Load deadline in seconds; 0 (default) waits until completion")


def operation(state, name, method, params=None, timeout=None, allow_error=False):
    if timeout is None:
        timeout = state["args"].operation_timeout
    state["phase"] = name
    begin = time.perf_counter()
    before = len(state["resources"])
    row = dict(kind="operation", name=name, method=method, params=params or {},
               start_seconds=begin-state["started"], timeout_seconds=timeout)
    try:
        payload = request(state, method, params, timeout)
        row["status"] = "rpc_error" if "error" in payload else "ok"
        row["response"] = payload
    except Exception as error:
        row["status"] = state.get("termination") or ("process_exit" if state["process"].poll() is not None else "transport_error")
        row["error"] = str(error)
        payload = {"error": {"message": str(error)}}
    row["seconds"] = time.perf_counter() - begin
    observed = state["resources"][before:]
    if observed:
        row["peak_private_bytes"] = max(s["private"] for s in observed)
        row["peak_rss_bytes"] = max(s["rss"] for s in observed)
        row["minimum_available_bytes"] = min(s["available"] for s in observed)
        row["cpu_seconds"] = observed[-1]["cpu_seconds"] - observed[0]["cpu_seconds"]
    append_json(state["directory"] / "operations.jsonl", row)
    state["operations"].append({k: v for k, v in row.items() if k != "response"})
    print(json.dumps(dict(case=state["case"], operation=name, status=row["status"], seconds=round(row["seconds"], 3))), flush=True)
    if "error" in payload:
        if allow_error and row["status"] == "rpc_error" and payload["error"].get("code") != -32001:
            return None
        raise RuntimeError(json.dumps(payload["error"]))
    return payload.get("result", {})


def frames(state, name, motion=None):
    # Capture/readback and metadata serialization are outside the timed window.
    proof = capture(state, name + "_validation")
    capture_path = state["directory"] / (name + "_validation.png")
    capture_ok = proof is not None and verify_capture(capture_path)
    state["phase"] = name + "_warmup"
    time.sleep(state["args"].warmup)
    state["phase"] = name
    scene = request(state, "stats", timeout=30).get("result", {})
    camera = request(state, "camera.get", timeout=30).get("result", {})
    operation(state, name + "_begin", "performance.begin")
    state["phase"] = name
    samples = []
    end = time.perf_counter() + state["args"].seconds
    index = 0
    while time.perf_counter() < end:
        if motion:
            method, values = motion(index)
            response = request(state, method, values, 30)
            if "error" in response:
                raise RuntimeError(str(response))
        begin = time.perf_counter()
        response = request(state, "performance.get", timeout=30)
        if "error" in response:
            raise RuntimeError(str(response))
        sample = response["result"]
        sample["midpoint"] = (begin + time.perf_counter()) / 2 - state["started"]
        samples.append(sample)
        index += 1
        time.sleep(.08)
    record = operation(state, name + "_end", "performance.end")
    record["runtime"] = {key: samples[0].get(key) for key in ("renderer", "sdlVersion", "version", "buildConfiguration")} if samples else {}
    record["host"] = dict(name=platform.node(), os=platform.platform(), machine=platform.machine())
    summary = dict(kind="frames", name=name, **summarize_capture(record, state["args"].width, state["args"].height, bool(motion)))
    summary["capture_verified"] = capture_ok
    summary["capture"] = capture_path.name
    summary["comparison_key"] = comparison_key(record, bool(motion))
    summary.update({k: v for k, v in summarize_frames(samples).items() if k.startswith("sampled_")})
    if not capture_ok:
        summary["valid"] = False
        summary["fps"] = None
        summary["exclusions"].append("capture_failed")
        for key in list(summary):
            if key.startswith("frame_interval_"):
                del summary[key]
    if not summary["valid"]:
        summary["measurement_note"] = "EXCLUDE: " + ", ".join(summary["exclusions"])
    state["frames"].append(summary)
    append_json(state["directory"] / "frames.jsonl", dict(**summary, scene=scene, camera=camera, raw=samples, recording=record))
    print(json.dumps(dict(case=state["case"], scenario=name, fps=summary["fps"])), flush=True)
    return summary


def capture(state, name):
    return operation(state, "capture_" + name, "screenshot.capture",
                     {"path": str(state["directory"] / (name + ".png"))}, timeout=90, allow_error=True)


def configure_view(state):
    operation(state, "camera_isometric", "camera.view", {"preset": "isometric"})
    operation(state, "camera_fit", "camera.frame")
    for helper in ("grid", "origin", "dimensions"):
        operation(state, "hide_" + helper, helper + ".set", {"visible": False})


def wait_annotations(state):
    state["phase"] = "annotation_preparation"
    begin = time.perf_counter()
    deadline = begin + state["args"].operation_timeout
    while time.perf_counter() < deadline:
        response = request(state, "status", timeout=15)
        if "error" in response:
            raise RuntimeError(str(response))
        status = response["result"]
        if status.get("annotationReady") or status.get("annotationPreparationError"):
            row = dict(kind="operation", name="annotation_preparation", status="ok" if status.get("annotationReady") else "preparation_failed",
                       seconds=time.perf_counter()-begin, response=status)
            append_json(state["directory"] / "operations.jsonl", row)
            state["operations"].append({k: v for k, v in row.items() if k != "response"})
            return
        time.sleep(.1)
    raise TimeoutError("Annotation preparation deadline exceeded")


def validation_suite(state, file_id, stats):
    configure_view(state)
    for scene, properties in ((True, False), (False, False), (False, True), (True, True)):
        name = f"panes_scene_{int(scene)}_properties_{int(properties)}"
        operation(state, name, "pane.set", {"visible": scene, "propertiesVisible": properties})
        frames(state, name)


def render_suite(state, file_id, stats):
    points_only = stats.get("pointCount", 0) > 0 and stats.get("triangleCount", 0) == 0 and stats.get("lineSegmentCount", 0) == 0
    configure_view(state)
    frames(state, "default_pane")
    operation(state, "pane_hide", "pane.set", {"visible": False})
    frames(state, "points_default" if points_only else "solid")
    capture(state, "points_default" if points_only else "solid")
    operation(state, "orbit_begin", "camera.frame")
    frames(state, "orbit", lambda i: ("camera.orbit", {"yawDegrees": 2, "pitchDegrees": .2 if i % 2 else -.2}))
    camera = operation(state, "camera_fit_motion", "camera.frame")["camera"]
    distance = camera["distance"]
    frames(state, "pan", lambda i: ("camera.pan", {"right": distance * (.002 if i % 2 else -.002)}))
    frames(state, "dolly", lambda i: ("camera.dolly", {"factor": 1.02 if i % 2 else 1/1.02}))
    operation(state, "fit_after_motion", "camera.frame")
    modes = () if points_only else (("edges", dict(solid=False, triangles=True, vertices=False)),
                        ("vertices", dict(solid=False, triangles=False, vertices=True)),
                        ("combined", dict(solid=True, triangles=True, vertices=True)))
    for name, flags in modes:
        operation(state, "enable_"+name, "render.set", dict(target=file_id, **flags))
        frames(state, name)
    operation(state, "points_small", "vertex-size.set", {"target": "scene", "pixels": 1})
    operation(state, "points_only", "render.set", {"target": file_id, "solid": False, "triangles": False, "vertices": True})
    frames(state, "vertices_1px")
    operation(state, "points_large", "vertex-size.set", {"target": "scene", "pixels": 8})
    frames(state, "vertices_8px")
    operation(state, "restore_points", "vertex-size.set", {"target": "scene", "pixels": 4})
    operation(state, "restore_solid", "render.set", {"target": file_id, "solid": True, "triangles": False, "vertices": points_only})
    operation(state, "transparent", "opacity.set", {"target": file_id, "value": .35})
    frames(state, "transparent_points" if points_only else "transparent")
    operation(state, "opaque", "opacity.set", {"target": file_id, "value": 1})
    uv = operation(state, "source_uv_grid", "render.set", {"target": file_id, "uvGrid": True}, allow_error=True)
    if uv is not None:
        frames(state, "source_uv_grid")
        operation(state, "source_uv_grid_off", "render.set", {"target": file_id, "uvGrid": False})
    operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
    frames(state, "hidden")
    operation(state, "show_source", "visibility.set", {"target": file_id, "visible": True})


def analysis_ready(state, target, name):
    return operation(state, name, "analysis.results", {"target": target})


def mesh_suite(state, file_id, stats):
    created = operation(state, "mesh_create", "analysis.create", {"type": "mesh", "a": file_id}, allow_error=True)
    if created is None:
        return
    target = created["target"]
    result = analysis_ready(state, target, "mesh_initial_ready")
    analysis_ready(state, target, "mesh_cached_results")
    operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
    operation(state, "analysis_zero_offset", "transform.set", {"target": target, "translation": [0, 0, 0]})
    configure_view(state)
    frames(state, "mesh_diagnostics")
    capture(state, "mesh_diagnostics")
    for metric in ("longest_edge", "equivalent_size", "shape", "size_jump"):
        operation(state, "quality_"+metric, "analysis.set", {"target": target, "mode": "surface_quality", "qualityMetric": metric})
        analysis_ready(state, target, "quality_"+metric+"_ready")
        frames(state, "quality_"+metric)
    operation(state, "topology_exact", "analysis.set", {"target": target, "topologyMode": "exact_position"})
    analysis_ready(state, target, "topology_exact_ready")
    operation(state, "topology_original", "analysis.set", {"target": target, "topologyMode": "original_index"})
    analysis_ready(state, target, "topology_original_ready")
    for name, values in (("degenerate_threshold", {"needleThresholdRatio": 500, "capMinAngleDegrees": 175}),
                         ("hole_threshold", {"holeSizeRatioTolerance": .1}),
                         ("fin_threshold", {"fins": True, "finMaxAreaRatio": .02})):
        operation(state, name, "analysis.set", dict(target=target, **values))
        analysis_ready(state, target, name+"_ready")
    for detector in ("boundary_edges", "non_manifold_edges", "inconsistently_oriented_tris", "duplicate_points",
                     "duplicate_tris", "degenerate_tris", "non_manifold_vertices", "holes", "fins"):
        operation(state, "findings_"+detector, "analysis.findings", {"target": target, "side": "a", "detector": detector, "limit": 10}, allow_error=True)
    operation(state, "mesh_export", "analysis.export", {"target": target, "path": str(state["directory"] / "analysis-report.json")}, allow_error=True)
    export_deadline = time.perf_counter() + state["args"].operation_timeout
    while time.perf_counter() < export_deadline:
        exported = operation(state, "mesh_export_status", "analysis.export-status", allow_error=True)
        if not exported or exported.get("export", {}).get("state") != "running":
            break
        time.sleep(.5)
    else:
        operation(state, "mesh_export_deadline_cancel", "analysis.export-cancel", allow_error=True)
        raise TimeoutError("Analysis export exceeded the operation deadline")
    operation(state, "analysis_offset", "transform.set", {"target": target, "translation": [1, 0, 0]})
    analysis_ready(state, target, "offset_cached_results")
    operation(state, "source_transform", "transform.set", {"target": file_id, "translation": [.1, 0, 0]})
    analysis_ready(state, target, "transform_invalidated_ready")


def intersection_suite(state, file_id, stats):
    target = operation(state, "intersection_create", "analysis.create", {"type": "mesh", "a": file_id})["target"]
    operation(state, "intersection_settings", "analysis.set", {"target": target, "selfIntersections": True,
                                                               "intersectionPairLimit": 10000, "intersectionCandidateLimit": 2000000})
    operation(state, "intersection_run", "analysis.run", {"target": target, "detector": "self_intersections"})
    time.sleep(.05)
    operation(state, "intersection_cancel", "analysis.cancel", {"target": target, "detector": "self_intersections"})
    analysis_ready(state, target, "intersection_cancel_ready")
    operation(state, "intersection_retry", "analysis.run", {"target": target, "detector": "self_intersections"})
    wait_detector(state, target, "self_intersections", "intersection_retry_ready")
    operation(state, "intersection_findings", "analysis.findings", {"target": target, "side": "a", "detector": "self_intersections", "limit": 10}, allow_error=True)


def wait_detector(state, target, detector, name):
    """Manual detector work can still run after analysis.results acknowledges."""
    begin = time.perf_counter()
    deadline = begin + state["args"].operation_timeout
    before = len(state["resources"])
    while time.perf_counter() < deadline:
        result = operation(state, name+"_poll", "analysis.results", {"target": target},
                           timeout=max(1, deadline-time.perf_counter()))
        detector_result = result.get("aToB", {}).get("detectors", {}).get(detector, {})
        if detector_result.get("status") in ("complete", "partial", "canceled", "failed", "unavailable"):
            row = dict(kind="operation", name=name, method="analysis.results", status="ok",
                       seconds=time.perf_counter()-begin, start_seconds=begin-state["started"],
                       detector_status=detector_result.get("status"), response={"result": result})
            observed = state["resources"][before:]
            if observed:
                row["peak_private_bytes"] = max(s["private"] for s in observed)
            append_json(state["directory"] / "operations.jsonl", row)
            state["operations"].append({k: v for k, v in row.items() if k != "response"})
            return result
        state["phase"] = name+"_waiting"
        time.sleep(2)
    operation(state, name+"_deadline_cancel", "analysis.cancel", {"target": target, "detector": detector}, allow_error=True)
    raise TimeoutError("Manual detector exceeded the operation deadline")


def uv_suite(state, file_id, stats):
    operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
    for kind in ("uv", "uv_quality"):
        created = operation(state, kind+"_create", "analysis.create", {"type": kind, "a": file_id}, allow_error=True)
        if created is None:
            continue
        target = created["target"]
        analysis_ready(state, target, kind+"_initial_ready")
        operation(state, kind+"_zero_offset", "transform.set", {"target": target, "translation": [0, 0, 0]})
        for name, settings in (("surface", {"uvView": "surface", "showEdges": False}),
                               ("surface_edges", {"showEdges": True}),
                               ("layout", {"uvView": "layout"}),
                               ("separated", {"uvSeparated": True})):
            operation(state, kind+"_"+name, "analysis.set", dict(target=target, **settings))
            analysis_ready(state, target, kind+"_"+name+"_ready")
            operation(state, kind+"_"+name+"_view", "camera.view", {"preset": "isometric" if name.startswith("surface") else "front"})
            operation(state, kind+"_"+name+"_fit", "camera.frame", {"object": target}, allow_error=True)
            frames(state, kind+"_"+name)
        capture(state, kind+"_separated")
        if kind == "uv":
            for name, settings in (("density", {"uvDensityU": 25, "uvDensityV": 25}), ("u", {"uvColor": "u"}), ("v", {"uvColor": "v"})):
                operation(state, "uv_"+name, "analysis.set", dict(target=target, **settings))
                analysis_ready(state, target, "uv_"+name+"_ready")
        else:
            for metric in ("angle", "area", "orientation", "anisotropy", "min_stretch", "overlap"):
                operation(state, "uv_metric_"+metric, "analysis.set", {"target": target, "uvMetric": metric})
                analysis_ready(state, target, "uv_metric_"+metric+"_ready")
            for name, settings in (("absolute", {"uvNormalization": "absolute"}),
                                   ("threshold", {"uvThresholdEnabled": True, "uvThreshold": 3}),
                                   ("range", {"uvRangeEnabled": True, "uvRangeMinimum": 1, "uvRangeMaximum": 4}),
                                   ("cross_patch_overlap", {"uvOverlapEnabled": True, "uvOverlapScope": "selected_patches"})):
                operation(state, "uv_"+name, "analysis.set", dict(target=target, **settings))
                analysis_ready(state, target, "uv_"+name+"_ready")
            page = operation(state, "uv_triangles_page", "analysis.uv-triangles", {"target": target, "offset": 0, "limit": 20}, allow_error=True)
            if page and page.get("items"):
                first = page["items"][0]
                if first.get("sourcePartId"):
                    operation(state, "uv_linked_probe", "analysis.uv-probe", {"target": target, "object": first["sourcePartId"], "index": first["triangle"], "barycentric": [1/3, 1/3, 1/3]}, allow_error=True)
                    operation(state, "uv_linked_probe_get", "analysis.uv-probe-get", {"target": target}, allow_error=True)
                    operation(state, "uv_linked_probe_clear", "analysis.uv-probe-clear", {"target": target}, allow_error=True)
            probe_valid_uv(state, target, stats)
        operation(state, kind+"_delete", "analysis.delete", {"target": target})


def probe_valid_uv(state, target, stats):
    # The first part can have no UVs even when the rest of a large mesh has them.
    # Retain both the first-page result and a successful linked-probe measurement.
    for fraction in (.25, .5, .75):
        offset = int(stats["triangleCount"] * fraction)
        page = operation(state, "uv_later_page_"+str(offset), "analysis.uv-triangles",
                         {"target": target, "offset": offset, "limit": 20}, allow_error=True)
        candidate = next((item for item in (page or {}).get("items", []) if item.get("valid")), None)
        if candidate:
            operation(state, "uv_valid_probe", "analysis.uv-probe",
                      {"target": target, "object": candidate["sourcePartId"], "index": candidate["triangle"], "barycentric": [1/3, 1/3, 1/3]})
            operation(state, "uv_valid_probe_get", "analysis.uv-probe-get", {"target": target})
            operation(state, "uv_valid_probe_clear", "analysis.uv-probe-clear", {"target": target})
            return


def uv_probe_suite(state, file_id, stats):
    target = operation(state, "uv_quality_create", "analysis.create", {"type": "uv_quality", "a": file_id})["target"]
    analysis_ready(state, target, "uv_quality_initial_ready")
    probe_valid_uv(state, target, stats)


def uv_display_suite(state, file_id, stats):
    operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
    configure_view(state)
    operation(state, "uv_up_axis", "up-axis.set", {"axis": "z"})
    target = operation(state, "uv_quality_create", "analysis.create", {"type": "uv_quality", "a": file_id})["target"]
    analysis_ready(state, target, "uv_quality_initial_ready")
    operation(state, "uv_zero_offset", "transform.set", {"target": target, "translation": [0, 0, 0]})
    for separated in (False, True):
        name = "separated" if separated else "overlapping"
        operation(state, "uv_layout_"+name, "analysis.set", {"target": target, "uvView": "layout", "uvSeparated": separated})
        analysis_ready(state, target, "uv_layout_"+name+"_ready")
        # UV layout is in world XZ; front with Z up looks perpendicular to it.
        operation(state, "uv_front", "camera.view", {"preset": "front"})
        operation(state, "uv_fit", "camera.frame", {"object": target})
        frames(state, name+"_whole")
        capture(state, name+"_whole")
        operation(state, "uv_zoom", "camera.dolly", {"factor": .15})
        frames(state, name+"_close")
        capture(state, name+"_close")
    probe_valid_uv(state, target, stats)


def comparison_suite(state, file_id, stats):
    target = operation(state, "comparison_create", "analysis.create", {"type": "surface_comparison", "a": file_id, "b": file_id})["target"]
    analysis_ready(state, target, "comparison_identical_ready")
    analysis_ready(state, target, "comparison_cached_results")
    operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
    for mode in ("distance", "overlay", "a", "b", "surface_quality"):
        operation(state, "comparison_mode_"+mode, "analysis.set", {"target": target, "mode": mode})
        analysis_ready(state, target, "comparison_mode_"+mode+"_ready")
        operation(state, "fit_"+mode, "camera.frame")
        frames(state, "comparison_"+mode)
    operation(state, "comparison_tolerance", "analysis.set", {"target": target, "tolerance": .1, "colorRange": 1})
    analysis_ready(state, target, "comparison_tolerance_ready")
    operation(state, "comparison_swap", "analysis.swap", {"target": target})
    analysis_ready(state, target, "comparison_swap_ready")
    capture(state, "comparison")


def distance_only_suite(state, file_id, stats):
    target = operation(state, "distance_only_create", "analysis.create", {"type": "surface_comparison", "a": file_id, "b": file_id})["target"]
    flags = ("autoUpdateBoundaries", "autoUpdateNonManifold", "autoUpdateWinding", "duplicatePoints",
             "duplicateTriangles", "degenerateTriangles", "nonManifoldVertices", "holes", "fins", "selfIntersections")
    operation(state, "distance_only_settings", "analysis.set", dict(target=target, **{name: False for name in flags}))
    result = operation(state, "distance_only_ready", "analysis.results", {"target": target}, allow_error=True)
    if result:
        operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
        configure_view(state)
        frames(state, "distance_without_diagnostics")
        capture(state, "distance_without_diagnostics")


def offset_comparison_suite(state, file_id, stats):
    copy = Path(state["working"].name) / "translated-copy.obj"
    fixture_copy(state["model"], copy)
    added = operation(state, "comparison_second_file", "model.add", {"path": str(copy)}, timeout=state["args"].load_timeout)
    if added.get("addedCount") != 1:
        raise RuntimeError(str(added))
    other = added["addedIds"][0]
    radius = stats["bounds"]["radius"]
    shift = max(.001, radius*.001)
    operation(state, "comparison_translate_b", "transform.set", {"target": other, "translation": [shift, 0, 0]})
    target = operation(state, "comparison_offset_create", "analysis.create", {"type": "surface_comparison", "a": file_id, "b": other})["target"]
    analysis_ready(state, target, "comparison_offset_ready")
    analysis_ready(state, target, "comparison_offset_cached")
    operation(state, "comparison_b_off", "analysis.enable", {"target": target, "side": "b", "enabled": False})
    analysis_ready(state, target, "comparison_b_off_ready")
    operation(state, "comparison_b_on", "analysis.enable", {"target": target, "side": "b", "enabled": True})
    analysis_ready(state, target, "comparison_b_on_ready")
    operation(state, "comparison_swap_offset", "analysis.swap", {"target": target})
    analysis_ready(state, target, "comparison_swap_offset_ready")
    capture(state, "comparison_offset")


def lifecycle_suite(state, file_id, stats):
    operation(state, "objects_inventory", "objects.list")
    operation(state, "scene_tree", "scene.tree")
    operation(state, "visibility_off", "visibility.set", {"target": file_id, "visible": False})
    operation(state, "undo_visibility", "scene.undo")
    operation(state, "redo_visibility", "scene.redo")
    operation(state, "visibility_on", "visibility.set", {"target": file_id, "visible": True})
    operation(state, "translate", "transform.set", {"target": file_id, "translation": [1, 2, 3]})
    operation(state, "reset_transform", "transform.reset", {"target": file_id})
    view = operation(state, "view_create", "view.create", {"name": "Stress baseline"})
    operation(state, "view_apply", "view.apply", {"viewId": view["viewId"]})
    scene_path = Path(state["working"].name) / "scene.woby"
    operation(state, "scene_save", "scene.save-as", {"path": str(scene_path)})
    operation(state, "scene_reopen_live", "scene.open", {"path": str(scene_path), "onDirty": "discard"}, timeout=state["args"].load_timeout)
    operation(state, "scene_new", "scene.new", {"onDirty": "discard"})
    operation(state, "scene_reopen_empty", "scene.open", {"path": str(scene_path)}, timeout=state["args"].load_timeout)
    operation(state, "reopened_stats", "stats")
    capture(state, "reopened")
    operation(state, "clear_after_reopen", "scene.new", {"onDirty": "discard"})
    time.sleep(2)
    operation(state, "empty_stats", "stats")


def annotation_suite(state, file_id, stats):
    objects = operation(state, "objects_inventory", "objects.list")["objects"]
    groups = [o for o in objects if o["kind"] == "group"]
    if not groups or not stats.get("triangleCount"):
        return
    configure_view(state)
    target = groups[0]["id"]
    operation(state, "annotation_fit", "camera.frame", {"object": target})
    annotations = []
    for preset in ("isometric", "front", "top", "right"):
        operation(state, "annotation_camera_"+preset, "camera.view", {"preset": preset})
        created = operation(state, "annotation_line_"+preset, "annotation.create",
                            {"target": target, "shape": "line", "start": [-.025, 0], "end": [.025, 0]}, allow_error=True)
        if created:
            annotations.append(created["target"])
            break
    if not annotations:
        return
    annotation = annotations[0]
    operation(state, "annotation_move", "annotation.move", {"target": annotation, "delta": [0, .01]}, allow_error=True)
    operation(state, "annotation_reshape", "annotation.reshape", {"target": annotation, "start": [-.04, 0], "end": [.04, 0]}, allow_error=True)
    operation(state, "annotation_style", "annotation.set", {"target": annotation, "width": 6, "rgb": [1, .4, .1], "comments": "Stress baseline"})
    operation(state, "annotation_get", "annotation.get", {"target": annotation})
    rectangle = operation(state, "annotation_rectangle", "annotation.create",
                          {"target": target, "shape": "rectangle", "start": [-.03, -.03], "end": [.03, .03]}, allow_error=True)
    if rectangle:
        annotations.append(rectangle["target"])
    frames(state, "annotations")
    capture(state, "annotations")
    operation(state, "annotation_list", "annotation.list")
    operation(state, "annotation_source_transform", "transform.set", {"target": file_id, "translation": [1, 0, 0]})
    wait_annotations(state)
    operation(state, "annotation_after_transform", "annotation.get", {"target": annotation})
    scene_path = Path(state["working"].name) / "annotations.woby"
    operation(state, "annotation_scene_save", "scene.save-as", {"path": str(scene_path)})
    operation(state, "annotation_scene_reopen", "scene.open", {"path": str(scene_path), "onDirty": "discard"}, timeout=state["args"].load_timeout)
    wait_annotations(state)
    restored = operation(state, "annotation_restored_list", "annotation.list")
    annotations = [item["id"] for item in restored["annotations"]]
    for item in annotations:
        operation(state, "annotation_delete", "annotation.delete", {"target": item})
    operation(state, "annotation_undo_delete", "scene.undo")
    operation(state, "annotation_redo_delete", "scene.redo")


def retention_suite(state, file_id, stats):
    operation(state, "clear_initial", "scene.new", {"onDirty": "discard"})
    for cycle in range(5):
        result = operation(state, f"cycle_{cycle}_add", "model.add", {"path": state["model"]}, timeout=state["args"].load_timeout)
        if result["addedCount"] != 1:
            raise RuntimeError(str(result))
        wait_annotations(state)
        current = result["addedIds"][0]
        operation(state, f"cycle_{cycle}_edges", "render.set", {"target": current, "triangles": True})
        operation(state, f"cycle_{cycle}_vertices", "render.set", {"target": current, "vertices": True})
        frames(state, f"cycle_{cycle}_combined")
        operation(state, f"cycle_{cycle}_remove", "model.remove", {"target": current})
        operation(state, f"cycle_{cycle}_clear_history", "scene.new", {"onDirty": "discard"})
        state["phase"] = f"cycle_{cycle}_empty"
        time.sleep(2)
        operation(state, f"cycle_{cycle}_empty_stats", "stats")


def batch_suite(state, file_id, stats):
    operation(state, "clear_initial", "scene.new", {"onDirty": "discard"})
    folder = Path(state["working"].name) / "models"
    folder.mkdir()
    for i in range(state["args"].batch_count):
        parent = folder / f"folder-{i % 4}"
        parent.mkdir(exist_ok=True)
        fixture_copy(state["model"], parent / f"model-{i:04}.obj")
    bad = state["args"].models / "pointclouds" / "testpoints.obj"
    if bad.is_file():
        fixture_copy(bad, folder / "missing-material.obj")
    for tree in (False, True):
        label = "tree" if tree else "flat"
        added = operation(state, "folder_"+label, "folder.add", {"path": str(folder), "tree": tree}, timeout=state["args"].load_timeout)
        operation(state, label+"_stats", "stats")
        operation(state, label+"_scene_tree", "scene.tree")
        configure_view(state)
        frames(state, label+"_pane_visible")
        operation(state, label+"_hide_pane", "pane.set", {"visible": False})
        frames(state, label+"_pane_hidden")
        operation(state, label+"_show_pane", "pane.set", {"visible": True})
        if added.get("addedIds"):
            operation(state, label+"_duplicate_path", "model.add", {"path": str(folder / "folder-0" / "model-0000.obj")})
        capture(state, "batch_"+label)
        operation(state, label+"_clear", "scene.new", {"onDirty": "discard"})


def controls_suite(state, file_id, stats):
    configure_view(state)
    initial = operation(state, "camera_initial", "camera.get")["camera"]
    for name, method, values in (("near", "camera.dolly", {"factor": .5}),
                                 ("far", "camera.dolly", {"factor": 8}),
                                 ("roll", "camera.roll", {"rollDegrees": 30}),
                                 ("move", "camera.move", {"right": initial["distance"]*.05, "forward": initial["distance"]*.05})):
        operation(state, name, method, values)
        frames(state, name)
    operation(state, "restore_camera", "camera.frame")
    for helper in ("grid", "origin", "dimensions"):
        operation(state, "show_"+helper, helper+".set", {"visible": True})
    frames(state, "helpers")
    for name, method, values in (("rotate", "transform.set", {"rotationDegrees": [10, 20, 30]}),
                                 ("scale", "transform.set", {"scale": 1.1}),
                                 ("color", "color.set", {"rgb": [.3, .6, .8]}),
                                 ("reset_color", "color.reset", {}),
                                 ("reset_transform", "transform.reset", {})):
        operation(state, name, method, dict(target=file_id, **values))
    if stats.get("lineSegmentCount", 0):
        for depth in (True, False):
            operation(state, "lines_"+str(depth), "render.set", {"target": file_id, "lineWidth": 8, "lineDepthTest": depth})
            frames(state, "wide_lines_depth_"+str(depth))
    for axis in ("y", "z"):
        operation(state, "up_axis_"+axis, "up-axis.set", {"axis": axis})
        operation(state, "fit_axis_"+axis, "camera.frame")
    view = operation(state, "saved_view", "view.create", {"name": "Controls"})["viewId"]
    operation(state, "view_update", "view.update", {"viewId": view})
    operation(state, "view_rename", "view.rename", {"viewId": view, "name": "Updated controls"})
    operation(state, "view_delete", "view.delete", {"viewId": view})


def concurrent_suite(state, file_id, stats):
    targets = []
    for kind in ("mesh", "uv_quality", "surface_comparison"):
        params = dict(type=kind, a=file_id)
        if kind == "surface_comparison":
            params["b"] = file_id
        created = operation(state, "concurrent_"+kind, "analysis.create", params, allow_error=True)
        if created:
            targets.append(created["target"])
    frames(state, "three_analyses_pending", lambda i: ("camera.orbit", {"yawDegrees": 1}))
    operation(state, "invalidate_queued_analyses", "transform.set", {"target": file_id, "translation": [.1, 0, 0]})
    for target in targets:
        analysis_ready(state, target, "concurrent_ready")
    frames(state, "three_analyses_ready")
    for target in targets:
        operation(state, "concurrent_delete", "analysis.delete", {"target": target})
    frames(state, "after_analysis_delete")


def analysis_display_suite(state, file_id, stats):
    target = operation(state, "mesh_create", "analysis.create", {"type": "mesh", "a": file_id})["target"]
    analysis_ready(state, target, "mesh_initial_ready")
    operation(state, "hide_source", "visibility.set", {"target": file_id, "visible": False})
    operation(state, "analysis_zero_offset", "transform.set", {"target": target, "translation": [0, 0, 0]})
    configure_view(state)
    for visible in (True, False):
        operation(state, "diagnostics_pane_"+str(visible), "pane.set", {"visible": visible})
        frames(state, "diagnostics_pane_"+str(visible))
    flags = ("showEdges", "showBoundaries", "showNonManifold", "showWinding", "showDuplicatePoints",
             "showDuplicateTriangles", "showDegenerateTriangles", "showNonManifoldVertices",
             "showHoles", "showFins", "showSelfIntersections")
    operation(state, "hide_diagnostic_overlays", "analysis.set", dict(target=target, **{name: False for name in flags}))
    analysis_ready(state, target, "hidden_overlays_cached_results")
    for mode in ("overlay", "surface_quality"):
        operation(state, "display_"+mode, "analysis.set", {"target": target, "mode": mode, "qualityMetric": "shape"})
        analysis_ready(state, target, "display_"+mode+"_ready")
        for visible in (True, False):
            operation(state, mode+"_pane_"+str(visible), "pane.set", {"visible": visible})
            frames(state, mode+"_no_overlays_pane_"+str(visible))
    capture(state, "quality_without_overlays")
    operation(state, "findings_later_page", "analysis.findings", {"target": target, "side": "a", "detector": "boundary_edges", "offset": 100, "limit": 10})
    operation(state, "focus_boundary", "analysis.focus", {"target": target, "side": "a", "detector": "boundary_edges", "index": 0}, allow_error=True)
    operation(state, "export_to_cancel", "analysis.export", {"target": target, "path": str(state["directory"] / "cancelled-report.json")})
    time.sleep(.05)
    operation(state, "export_cancel", "analysis.export-cancel")
    for attempt in range(100):
        result = operation(state, "export_cancel_status", "analysis.export-status")
        if result.get("export", {}).get("state") != "running":
            break
        time.sleep(.1)


def analysis_lifecycle_suite(state, file_id, stats):
    targets = []
    for kind in ("mesh", "uv_quality", "surface_comparison"):
        params = dict(type=kind, a=file_id)
        if kind == "surface_comparison":
            params["b"] = file_id
        target = operation(state, "persist_create_"+kind, "analysis.create", params)["target"]
        analysis_ready(state, target, "persist_ready_"+kind)
        targets.append(target)
    scene_path = Path(state["working"].name) / "analyses.woby"
    operation(state, "analyses_save", "scene.save-as", {"path": str(scene_path)})
    operation(state, "analyses_reopen", "scene.open", {"path": str(scene_path), "onDirty": "discard"}, timeout=state["args"].load_timeout)
    objects = operation(state, "reopened_objects", "objects.list")["objects"]
    for item in objects:
        if item["kind"] == "analysis":
            analysis_ready(state, item["id"], "reopened_analysis_ready")
    operation(state, "reopened_tree", "scene.tree")
    capture(state, "reopened_analyses")
    operation(state, "clear_persisted_analyses", "scene.new", {"onDirty": "discard"})


def analysis_members_suite(state, file_id, stats):
    objects = operation(state, "objects_inventory", "objects.list")["objects"]
    groups = [item["id"] for item in objects if item["kind"] == "group"]
    if len(groups) < 2:
        raise ValueError("Member workload requires at least two source groups")
    target = operation(state, "member_create", "analysis.create", {"type": "mesh", "a": groups[0]})["target"]
    analysis_ready(state, target, "one_group_ready")
    operation(state, "member_add", "analysis.add", {"target": target, "side": "a", "object": groups[1]})
    analysis_ready(state, target, "two_groups_ready")
    operation(state, "member_disable", "analysis.enable", {"target": target, "side": "a", "object": groups[1], "enabled": False})
    analysis_ready(state, target, "member_disabled_ready")
    operation(state, "member_isolate", "analysis.enable", {"target": target, "side": "a", "object": groups[1], "enabled": True, "isolate": True})
    analysis_ready(state, target, "member_isolated_ready")
    operation(state, "member_clear", "analysis.clear", {"target": target, "side": "a"})
    operation(state, "member_add_file", "analysis.add", {"target": target, "side": "a", "object": file_id})
    analysis_ready(state, target, "whole_file_ready")
    operation(state, "member_remove_group", "analysis.remove", {"target": target, "side": "a", "object": groups[0]})
    analysis_ready(state, target, "member_removed_ready")
    operation(state, "member_transform", "transform.set", {"target": groups[1], "translation": [.01, 0, 0]})
    analysis_ready(state, target, "member_transform_ready")
    operation(state, "member_delete", "analysis.delete", {"target": target})


SUITES = {"load": None, "startup": None, "validation": validation_suite, "render": render_suite, "mesh": mesh_suite, "uv": uv_suite,
          "intersections": intersection_suite, "comparison": comparison_suite, "lifecycle": lifecycle_suite,
          "annotation": annotation_suite, "retention": retention_suite, "batch": batch_suite,
          "controls": controls_suite, "concurrent": concurrent_suite, "comparison-offset": offset_comparison_suite,
          "analysis-display": analysis_display_suite, "uv-probe": uv_probe_suite, "uv-display": uv_display_suite,
          "analysis-lifecycle": analysis_lifecycle_suite, "distance-only": distance_only_suite,
          "analysis-members": analysis_members_suite}


def run_case(args, model, suite, round_index):
    case = f"{suite}-{model.stem}-r{round_index}"
    directory = args.output / case
    directory.mkdir(exist_ok=False)
    working = tempfile.TemporaryDirectory(prefix="fixture-", dir=args.output)
    load_model = model
    if suite in ("lifecycle", "analysis-lifecycle", "annotation"):
        load_model = Path(working.name) / model.name
        fixture_copy(model, load_model)
    instance = "stress-" + uuid.uuid4().hex[:16]
    command = [str(args.executable), "--instance", instance, "--log-level", "info", "--log-file", str(directory / "viewer.log"),
               "--log-performance", "--log-frame-interval", "120", "--log-slow-frame-ms", "250"]
    if args.headless:
        command.append("--headless")
    else:
        command.extend(["--drawable-size", f"{args.width}x{args.height}"])
        if not args.visible_window:
            command.append("--hidden-window")
    if suite == "startup":
        command.extend(["--file", str(model)])
    startup = subprocess.STARTUPINFO() if os.name == "nt" else None
    if startup:
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 1 if args.visible_window else 0
    console = (directory / "console.log").open("w", encoding="utf-8")
    started = time.perf_counter()
    process = subprocess.Popen(command, cwd=working.name, stdout=console, stderr=console, startupinfo=startup)
    state = dict(case=case, directory=directory, working=working, args=args, process=process, started=started,
                 model=str(model),
                 phase="startup", resources=[], operations=[], frames=[], request_id=0, stop=threading.Event(),
                 cap=args.max_private_gib * GIB, floor=args.min_available_gib * GIB)
    watcher = threading.Thread(target=monitor, args=(state,), daemon=True)
    watcher.start()
    summary = dict(case=case, suite=suite, round=round_index, model=str(model), bytes=model.stat().st_size,
                   executable=str(args.executable), executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                   headless=args.headless, status="started")
    write_json(directory / "case.json", summary)
    try:
        registry = Path(os.environ["LOCALAPPDATA"]) / "woby" / "instances" / ("instance-"+instance+".json")
        startup_timeout = args.load_timeout if suite == "startup" else 45
        deadline = time.perf_counter() + startup_timeout if startup_timeout else math.inf
        while time.perf_counter() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"Viewer startup exited {process.returncode}")
            try:
                record = json.loads(registry.read_text(encoding="utf-8"))
                state["url"] = f"http://127.0.0.1:{record['port']}/rpc"
                state["token"] = record["token"]
                ready = request(state, "instance.info", timeout=2)
                if ready.get("result", {}).get("ready"):
                    break
            except (OSError, ValueError):
                pass
            time.sleep(.1)
        else:
            raise TimeoutError("Startup deadline exceeded")
        summary["startup_seconds"] = time.perf_counter() - started
        summary["status_before"] = operation(state, "status_before", "status")
        if not args.headless:
            operation(state, "initial_panes", "pane.set", {"visible": True, "propertiesVisible": False})
        if suite == "startup":
            objects = operation(state, "startup_inventory", "objects.list")
            loaded_ids = [item["id"] for item in objects["objects"] if item["kind"] == "file"]
            loaded = len(loaded_ids) == 1
        else:
            added = operation(state, "model_add", "model.add", {"path": str(load_model)}, timeout=args.load_timeout)
            summary["load_outcome"] = added
            loaded_ids = added.get("addedIds", [])
            loaded = added.get("addedCount") == 1 and added.get("failedCount") == 0
        if not loaded:
            summary["status"] = "load_failed"
        else:
            file_id = loaded_ids[0]
            summary["stats"] = operation(state, "loaded_stats", "stats")
            summary["status_after"] = operation(state, "status_after", "status")
            wait_annotations(state)
            if SUITES[suite]:
                SUITES[suite](state, file_id, summary["stats"])
            else:
                configure_view(state)
                if not args.headless:
                    frames(state, "loaded_default")
                capture(state, "loaded")
            summary["status"] = "completed"
        operation(state, "quit", "quit", {"onDirty": "discard"}, timeout=20)
        process.wait(timeout=20)
    except Exception as error:
        summary["status"] = state.get("termination") or ("process_exit" if process.poll() is not None else "failed")
        summary["error"] = str(error)
    finally:
        if process.poll() is None and summary.get("status") in ("failed", "process_exit"):
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
        summary["exit_code_before_cleanup"] = process.poll()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=20)
            summary["forced_cleanup"] = True
        state["stop"].set()
        watcher.join(timeout=3)
        console.close()
        working.cleanup()
        summary["exit_code"] = process.returncode
        exception_exit = 0xC0000000 <= process.returncode < 0xE0000000
        if not state.get("termination") and (exception_exit or
                (process.returncode not in (0, 1) and not summary.get("forced_cleanup"))):
            summary["status"] = "process_exit"
            if exception_exit and summary.get("forced_cleanup"):
                summary["classification_note"] = "Windows exception exit code recorded while harness cleanup was attempted; exit/cleanup ordering is uncertain"
        summary["total_seconds"] = time.perf_counter()-started
        summary["operations"] = state["operations"]
        summary["frames"] = state["frames"]
        if state["resources"]:
            summary["peak_private_bytes"] = max(s["private"] for s in state["resources"])
            summary["peak_rss_bytes"] = max(s["peak_rss"] for s in state["resources"])
            summary["minimum_available_bytes"] = min(s["available"] for s in state["resources"])
        write_json(directory / "resources.json", state["resources"])
        write_json(directory / "case.json", summary)
        append_json(args.output / "cases.jsonl", summary)
        print(json.dumps(dict(case=case, status=summary["status"], total_seconds=round(summary["total_seconds"], 3))), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("models", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--suite", choices=SUITES, default="load")
    parser.add_argument("--select", action="append", help="Filename substring; repeat for several models")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--seconds", type=float, default=4)
    parser.add_argument("--warmup", type=float, default=1.5)
    add_load_timeout_argument(parser)
    parser.add_argument("--operation-timeout", type=int, default=240)
    add_memory_limit_arguments(parser)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--visible-window", action="store_true", default=True, help="Show the desktop window (default)")
    parser.add_argument("--hidden-window", dest="visible_window", action="store_false", help="Hidden-window control; excluded from desktop FPS")
    parser.add_argument("--width", type=int, default=1280, help="Required drawable width in pixels")
    parser.add_argument("--height", type=int, default=720, help="Required drawable height in pixels")
    parser.add_argument("--batch-count", type=int, default=8)
    args = parser.parse_args()
    for name in ("executable", "models", "output"):
        setattr(args, name, getattr(args, name).resolve())
    if args.rounds < 1 or args.seconds < 1 or args.warmup < 0 or args.load_timeout < 0 or args.operation_timeout < 1:
        parser.error("Positive rounds, measurement seconds, and operation deadlines are required; load timeout may be 0.")
    if not all(math.isfinite(value) for value in (args.seconds, args.warmup)):
        parser.error("Finite measurement settings are required.")
    if not 1 <= args.batch_count <= 512:
        parser.error("Batch count must be 1..512")
    if not 1 <= args.width <= 16384 or not 1 <= args.height <= 16384:
        parser.error("Drawable dimensions must be 1..16384")
    if args.headless and args.suite not in ("load", "lifecycle"):
        parser.error("Headless mode does not continuously draw the desktop scene; use load/lifecycle only.")
    models = sorted(args.models.rglob("*.obj"), key=lambda p: p.stat().st_size)
    models = [p for p in models if "sources" not in p.relative_to(args.models).parts]
    if args.select:
        models = [p for p in models if any(s in p.name for s in args.select)]
    if not models:
        parser.error("No selected OBJ models")
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = dict(arguments={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                    harness_version=4, harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    python=sys.version, psutil=psutil.__version__,
                    os=platform.platform(), machine=platform.machine(),
                    executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                    started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    models=[dict(path=str(p), bytes=p.stat().st_size, mtime_ns=p.stat().st_mtime_ns) for p in models],
                    physical_memory=psutil.virtual_memory().total,
                    cache_policy="Fresh viewer per case; warmup before each window. OS file cache and driver caches are uncontrolled, not cold.",
                    frame_sampling="Bounded complete frame events (16384 max); overflow/mismatches excluded. Completion-interval tails only; no GPU percentiles.")
    write_json(args.output / "manifest.json", manifest)
    shutil.copyfile(Path(__file__), args.output / "harness-source.py")
    gpu_file = (args.output / "gpu.csv").open("w", encoding="utf-8")
    gpu_process = None
    try:
        try:
            gpu_process = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,name,driver_version,memory.used,utilization.gpu,utilization.memory,temperature.gpu,power.draw,clocks.current.graphics,clocks.current.memory",
                                            "--format=csv,nounits", "-lms", "1000"], stdout=gpu_file, stderr=subprocess.DEVNULL,
                                           creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        except OSError:
            pass
        for round_index in range(args.rounds):
            for model in (models if round_index % 2 == 0 else list(reversed(models))):
                run_case(args, model, args.suite, round_index)
    finally:
        if gpu_process is not None:
            gpu_process.terminate()
            gpu_process.wait(timeout=10)
        gpu_file.close()


if __name__ == "__main__":
    main()
