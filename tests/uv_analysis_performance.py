"""Opt-in real-viewer UV performance measurements; no timing assertions.

Use a Release executable. Run without concurrent builds/tests or other viewers.
Desktop rendering includes VSync, the scene pane, and the default window size.
CLI operation latency includes process/RPC overhead; results waits include CPU
analysis and GPU buffer preparation. Screenshots are outside timed intervals.
Raw observations and screenshots are retained in the requested output directory.

Example from the repository root:
    uv run tests/uv_analysis_performance.py build/vs2026-vcpkg/bin/Release/woby.exe MODEL.obj build/uv-perf

Each round uses a fresh viewer with two seconds of warm-up per scenario. Source
geometry is hidden when measuring a separate analysis; that analysis remains
selected with the default panes visible. Layouts omit parts without UVs and use
a front camera fitted to their own bounds, while surfaces use an isometric view.
Quality metric edits are measured in the separated layout. GPU timings are the
backend's asynchronous whole-frame timestamps. FPS uses frame-counter deltas
over query midpoints, not the reciprocal of sampled frame times. Samples are
sparse and must not be interpreted as a complete frame-time/P99 distribution.
"""

import argparse
import json
import statistics
import subprocess
import tempfile
import time
import uuid
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=6)
    args = parser.parse_args()
    executable, model, output = (p.resolve() for p in (args.executable, args.model, args.output))
    if args.rounds < 1 or args.seconds < 2:
        parser.error("Use at least one round and two seconds per measurement.")
    output.mkdir(parents=True, exist_ok=False)
    observations = []
    for round_index in range(args.rounds):
        instance = "uv-perf-" + uuid.uuid4().hex[:12]
        log_path = output / f"round-{round_index}.log"
        with tempfile.TemporaryDirectory(prefix="woby-uv-performance-") as directory:
            root = Path(directory).resolve()
            with (output / f"round-{round_index}-console.log").open("w", encoding="utf-8") as log:
                viewer = subprocess.Popen(
                    [str(executable), "--instance", instance, "--log-level", "info",
                     "--log-file", str(log_path), "--log-performance",
                     "--log-frame-interval", "120", "--log-slow-frame-ms", "40"],
                    cwd=root, stdout=log, stderr=log)

                def ctl(*command):
                    result = subprocess.run(
                        [str(executable), "ctl", "--instance", instance, *map(str, command),
                         "--json", "--timeout", "180"], cwd=root,
                        capture_output=True, text=True, encoding="utf-8", timeout=190)
                    if result.returncode:
                        raise RuntimeError((command, result.stdout, result.stderr))
                    return json.loads(result.stdout)

                def operation(name, command, analysis=None):
                    start = time.perf_counter()
                    result = ctl(*command)
                    command_ms = (time.perf_counter() - start) * 1000
                    if analysis:
                        ctl("analysis", "results", analysis)
                    entry = dict(round=round_index, operation=name, command_ms=command_ms,
                                 ready_ms=(time.perf_counter() - start) * 1000)
                    observations.append(entry)
                    print(json.dumps(entry), flush=True)
                    return result

                def measure(name, target, layout=False):
                    ctl("camera", "view", "front" if layout else "isometric")
                    operation(name + "_frame_camera", ("camera", "frame", "--object", target))
                    time.sleep(2)
                    samples = []
                    deadline = time.perf_counter() + args.seconds
                    while time.perf_counter() < deadline:
                        begin = time.perf_counter()
                        sample = ctl("performance", "get")
                        sample["query_midpoint"] = (begin + time.perf_counter()) / 2
                        if not samples or sample["frameIndex"] != samples[-1]["frameIndex"]:
                            samples.append(sample)
                        time.sleep(.1)
                    if len(samples) < 2:
                        raise RuntimeError("Too few distinct rendered frames")
                    first, last = samples[0], samples[-1]
                    entry = dict(round=round_index, scenario=name, samples=samples,
                                 fps=(last["frameIndex"] - first["frameIndex"]) /
                                     (last["query_midpoint"] - first["query_midpoint"]),
                                 median_frame_ms=statistics.median(s["frameMilliseconds"] for s in samples),
                                 median_stages_ms={k: statistics.median(s["stagesMilliseconds"][k] for s in samples)
                                                   for k in first["stagesMilliseconds"]},
                                 camera=ctl("camera", "get"))
                    gpu = [s["gpuFrameMilliseconds"] for s in samples if s["gpuFrameMilliseconds"] is not None]
                    entry["median_gpu_ms"] = statistics.median(gpu) if gpu else None
                    observations.append(entry)
                    print(json.dumps({k: v for k, v in entry.items() if k not in ("samples", "camera")}), flush=True)
                    if round_index == 0:
                        ctl("screenshot", output / (name + ".png"))
                    (output / "measurements.json").write_text(json.dumps(observations, indent=2), encoding="utf-8")

                try:
                    deadline = time.monotonic() + 45
                    while True:
                        if viewer.poll() is not None:
                            raise RuntimeError("Viewer exited before readiness")
                        discovered = subprocess.run([str(executable), "ctl", "instances", "--json"],
                                                    capture_output=True, text=True, timeout=10)
                        if discovered.returncode == 0 and any(
                                i["id"] == instance and i["ready"] for i in json.loads(discovered.stdout)):
                            break
                        if time.monotonic() > deadline:
                            raise TimeoutError("Viewer startup")
                        time.sleep(.05)
                    added = operation("import", ("model", "add", model))
                    file_id = added["addedIds"][0]
                    metadata = dict(model=str(model), model_bytes=model.stat().st_size,
                                    executable=str(executable), stats=ctl("stats"), status=ctl("status"))
                    (output / f"metadata-{round_index}.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
                    ctl("up-axis", "set", "y")
                    for helper in ("grid", "origin", "dimensions"):
                        ctl(helper, "set", "--visible", "false")
                    measure("solid", file_id)
                    ctl("render", "set", file_id, "--uv-grid", "true")
                    measure("source_grid", file_id)
                    ctl("render", "set", file_id, "--uv-grid", "false")
                    ctl("visibility", "set", file_id, "--visible", "false")
                    # Reverse analysis order on alternating rounds to reduce order bias.
                    for kind in (("uv", "uv_quality") if round_index % 2 == 0 else ("uv_quality", "uv")):
                        start = time.perf_counter()
                        analysis = ctl("analysis", "create", "--type", kind, "--a", file_id)["target"]
                        result = ctl("analysis", "results", analysis)
                        entry = dict(round=round_index, operation=kind + "_create", ready_ms=(time.perf_counter()-start)*1000)
                        observations.append(entry)
                        print(json.dumps(entry), flush=True)
                        (output / f"{kind}-results-{round_index}.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
                        ctl("transform", "set", analysis, "--translation", 0, 0, 0)
                        operation(kind + "_surface", ("analysis", "set", analysis, "--uv-view", "surface",
                                  "--show-edges", "false"), analysis)
                        measure(kind + "_surface", analysis)
                        ctl("analysis", "set", analysis, "--show-edges", "true")
                        measure(kind + "_surface_edges", analysis)
                        operation(kind + "_layout", ("analysis", "set", analysis, "--uv-view", "layout"), analysis)
                        measure(kind + "_layout_edges", analysis, True)
                        operation(kind + "_separated", ("analysis", "set", analysis, "--uv-separated", "true"), analysis)
                        measure(kind + "_separated_edges", analysis, True)
                        if kind == "uv_quality":
                            for metric in ("area", "orientation", "angle"):
                                operation("metric_" + metric, ("analysis", "set", analysis, "--uv-metric", metric), analysis)
                        else:
                            operation("grid_density", ("analysis", "set", analysis, "--uv-density-u", 20), analysis)
                            operation("grid_color", ("analysis", "set", analysis, "--uv-color", "u"), analysis)
                        ctl("analysis", "delete", analysis)
                    ctl("quit", "--on-dirty", "discard")
                    if viewer.wait(timeout=15) != 0:
                        raise RuntimeError("Viewer did not exit successfully")
                finally:
                    (output / "measurements.json").write_text(json.dumps(observations, indent=2), encoding="utf-8")
                    if viewer.poll() is None:
                        viewer.kill()
                        viewer.wait(timeout=15)


if __name__ == "__main__":
    main()
