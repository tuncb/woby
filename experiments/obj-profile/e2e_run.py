# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Observe a fresh viewer from model.add through display, readiness, and rendering."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.request
import uuid

import psutil
import pynvml
from PIL import Image

from memory_map import read_process_map
from e2e_system import open_counters, sample_counters, close_counters


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def load_trace(path):
    if not path.exists():
        return []
    result = []
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    for index, line in enumerate(lines):
        try:
            result.append(json.loads(line))
        except ValueError:
            if index != len(lines) - 1 or text.endswith("\n"):
                raise ValueError("Malformed complete trace record")
            # The live observer can see a partially written final line.
    return result


def image_stats(path):
    with Image.open(path) as original:
        image = original.convert("RGB")
        colors = image.getcolors(image.width * image.height)
        return {"width": image.width, "height": image.height, "unique_colors": len(colors),
                "sha256": digest(path), "non_background_fraction":
                1 - max(count for count, _ in colors) / (image.width * image.height)}


def wrap(command, profiler, output, event_set):
    if profiler == "uprof":
        from deep_tools import SETS  # Loaded only in a uv environment with these dependencies.
        result = [r"C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe", "collect"]
        for event, mask, interval in SETS[event_set]:
            result += ["-e", f"event=pmcx{event:03x},umask=0x{mask:x},interval={interval},user=1,os=0"]
        return result + ["-o", str(output / "cpu")] + command
    if profiler in ("nsys", "nsys-gpu", "nsys-wddm", "nsys-wddm-full"):
        trace = {"nsys": "nvtx", "nsys-gpu": "nvtx,vulkan", "nsys-wddm": "nvtx,wddm", "nsys-wddm-full": "nvtx,wddm"}[profiler]
        result = [r"C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe",
                "profile", "--trace=" + trace, "--sample=none", "--cpuctxsw=none", "--wait=all",
                "--export=sqlite", "--output=" + str(output / "timeline")]
        if profiler == "nsys-wddm-full":
            result += ["--wddm-memory-trace=false", "--wddm-additional-events=true", "--wddm-backtraces=true"]
        return result + command
    if profiler == "vtune":
        return [r"C:\Program Files (x86)\Intel\oneAPI\vtune\2025.3\bin64\vtune.exe",
                "-collect", "hotspots", "-knob", "sampling-mode=sw", "-knob", "enable-stack-collection=true",
                "-result-dir", str(output / "vtune"), "--"] + command
    if profiler == "timechart":
        return [r"C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe", "timechart",
                "-e", "frequency,power", "-t", "100", "-o", str(output / "timechart")] + command
    if profiler == "ngfx":
        return [r"C:\Program Files\NVIDIA Corporation\Nsight Graphics 2026.3.1\host\windows-desktop-nomad-x64\ngfx.exe",
                "--activity=GPU Trace Profiler", "--platform=Windows (x86_64)", "--exe=" + command[0],
                "--args=" + subprocess.list2cmdline(command[1:]), "--start-after-ms=55000", "--max-duration-ms=2000",
                "--auto-export", "--output-dir=" + str(output)]
    return command


def run(args):
    exe, model, output = args.executable.resolve(strict=True), args.model.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "result.json").exists():
        raise ValueError("An existing result must not be overwritten")
    trace_path = output / "phases.jsonl"
    report = {"method": "Fresh viewer; warm file cache unless explicitly unwarmed; asynchronous model.add; separate display and ready endpoints",
              "executable": str(exe), "executable_sha256": digest(exe), "model": str(model),
              "model_bytes": model.stat().st_size, "hidden": args.hidden, "profiler": args.profiler,
              "settle_seconds": args.settle_seconds, "events": [], "samples": [], "status": [], "rpc": []}
    gpu = None
    try:
        pynvml.nvmlInit()
        gpu = pynvml.nvmlDeviceGetHandleByIndex(0)
        report["gpu"] = {"name": pynvml.nvmlDeviceGetName(gpu), "memory_total": pynvml.nvmlDeviceGetMemoryInfo(gpu).total,
                         "driver": pynvml.nvmlSystemGetDriverVersion(), "samples": [],
                         "scope": "Device-wide; includes other applications; utilization is not engine occupancy"}
    except pynvml.NVMLError as error:
        report["gpu_error"] = str(error)
    if not args.unwarmed:
        with model.open("rb") as source:
            for _ in iter(lambda: source.read(8 * 1024 * 1024), b""):
                pass
    report["cache_control"] = "not warmed; existing cache state unknown" if args.unwarmed else "sequentially read before launch"
    instance = "e2e-" + uuid.uuid4().hex[:12]
    command = [str(exe), "--instance", instance, "--drawable-size", "1280x720",
               "--log-file", str(output / "viewer.log"), "--log-level", "info"]
    if args.hidden:
        command.append("--hidden-window")
    if args.staging_mib:
        command += ["--upload-buffer-mib", str(args.staging_mib)]
    command = wrap(command, args.profiler, output, args.event_set)
    report["command"] = command
    epoch = time.perf_counter()
    report["observer_epoch_qpc_ns"] = int(epoch * 1e9)
    report["observer_epoch_utc"] = datetime.now(timezone.utc).isoformat()
    report["timezone_offset_seconds"] = datetime.now().astimezone().utcoffset().total_seconds()
    now = lambda: (time.perf_counter() - epoch) * 1000
    stopped = threading.Event()
    registry = {}
    request_number = 0
    request_lock = threading.Lock()
    target_pid = None
    process = None
    errors = []
    system_counters = open_counters()
    report["system_counter_paths"] = system_counters.get("paths", {})
    report["system_counter_errors"] = system_counters.get("errors", system_counters.get("error"))
    report["system_samples"] = []
    environment = dict(os.environ, WOBY_E2E_TRACE_PATH=str(trace_path), VK_LOADER_LAYERS_DISABLE="~implicit~")
    environment["VK_LOADER_LAYERS_ALLOW"] = "VK_LAYER_NV_nsight*,VK_LAYER_NV_GPU_Trace*,VK_LAYER_NV_nomad*" if args.profiler else ""
    # A native visible measurement needs a normal window. Profiler helpers remain hidden.
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0 if args.hidden or args.profiler else 1

    def rpc(method, params=None, timeout=60):
        nonlocal request_number
        with request_lock:
            request_number += 1
            identifier = request_number
        payload = dict(params or {})
        if method not in ("instance.info", "command.get"):
            payload["timeoutSeconds"] = timeout
        begin = now()
        request = urllib.request.Request(f"http://127.0.0.1:{registry['port']}/rpc",
            data=json.dumps({"jsonrpc": "2.0", "id": identifier, "method": method, "params": payload}).encode(),
            headers={"Content-Type": "application/json", "Authorization": "Bearer " + registry["token"]})
        with urllib.request.urlopen(request, timeout=timeout + 5) as response:
            value = json.load(response)
        report["rpc"].append({"id": identifier, "method": method, "start_ms": begin, "end_ms": now(), "error": value.get("error")})
        if "error" in value:
            raise RuntimeError(f"{method}: {value['error']}")
        return value["result"]

    def checkpoint(name):
        if not args.memory_map:
            return
        snapshot = read_process_map(target_pid, resident=True)
        memory = psutil.Process(target_pid).memory_info()
        snapshot.update(name=name, time_ms=now(), private_bytes=memory.private, working_set_bytes=memory.rss)
        report.setdefault("memory_maps", []).append(snapshot)

    def observe():
        last_system_sample = -1000.0
        unavailable_gpu_queries = set()
        while not stopped.wait(.10):
            try:
                if target_pid is None:
                    continue
                probe = psutil.Process(target_pid)
                with probe.oneshot():
                    memory = probe.memory_info(); cpu = probe.cpu_times(); io = probe.io_counters()
                    sample = {"time_ms": now(), "cpu_ms": (cpu.user + cpu.system) * 1000,
                              "user_ms": cpu.user * 1000, "kernel_ms": cpu.system * 1000,
                              "working_set": memory.rss, "private_bytes": memory.private,
                              "peak_working_set": memory.peak_wset, "peak_commit": memory.peak_pagefile,
                              "faults": memory.num_page_faults, "threads": probe.num_threads(),
                              "read_bytes": io.read_bytes, "write_bytes": io.write_bytes,
                              "read_ops": io.read_count, "write_ops": io.write_count,
                              "system_available_memory": psutil.virtual_memory().available,
                              "thread_cpu": [{"id": t.id, "cpu_ms": (t.user_time + t.system_time) * 1000} for t in probe.threads()]}
                    report["samples"].append(sample)
                if now() - last_system_sample >= 1000:
                    last_system_sample = now()
                    report["system_samples"].append({"time_ms": now(), "values": sample_counters(system_counters, target_pid)})
                if gpu and len(report["samples"]) % 2 == 0:
                    try:
                        usage = pynvml.nvmlDeviceGetUtilizationRates(gpu)
                        gpu_sample = {"time_ms": now(), "gpu_percent": usage.gpu,
                            "memory_percent": usage.memory, "memory_used": pynvml.nvmlDeviceGetMemoryInfo(gpu).used,
                            "sm_clock_MHz": pynvml.nvmlDeviceGetClockInfo(gpu, pynvml.NVML_CLOCK_SM),
                            "memory_clock_MHz": pynvml.nvmlDeviceGetClockInfo(gpu, pynvml.NVML_CLOCK_MEM),
                            "temperature_C": pynvml.nvmlDeviceGetTemperature(gpu, pynvml.NVML_TEMPERATURE_GPU),
                            "power_W": pynvml.nvmlDeviceGetPowerUsage(gpu) / 1000,
                            "pstate": pynvml.nvmlDeviceGetPerformanceState(gpu)}
                        optional = {
                            "pcie_rx_KB_per_second": lambda: pynvml.nvmlDeviceGetPcieThroughput(gpu, pynvml.NVML_PCIE_UTIL_RX_BYTES),
                            "pcie_tx_KB_per_second": lambda: pynvml.nvmlDeviceGetPcieThroughput(gpu, pynvml.NVML_PCIE_UTIL_TX_BYTES),
                            "pcie_link_generation": lambda: pynvml.nvmlDeviceGetCurrPcieLinkGeneration(gpu),
                            "pcie_link_width": lambda: pynvml.nvmlDeviceGetCurrPcieLinkWidth(gpu),
                            "clock_event_reasons": lambda: getattr(pynvml, "nvmlDeviceGetCurrentClocksEventReasons", pynvml.nvmlDeviceGetCurrentClocksThrottleReasons)(gpu),
                        }
                        for name, query in optional.items():
                            if name in unavailable_gpu_queries:
                                continue
                            try:
                                gpu_sample[name] = query()
                            except (pynvml.NVMLError, AttributeError) as error:
                                unavailable_gpu_queries.add(name)
                                report["gpu"].setdefault("unavailable_queries", {})[name] = str(error)
                        report["gpu"]["samples"].append(gpu_sample)
                    except pynvml.NVMLError as error:
                        errors.append(str(error))
            except psutil.NoSuchProcess:
                return
            except (OSError, AttributeError) as error:
                errors.append(str(error)); return

    watcher = threading.Thread(target=observe, daemon=True)
    try:
        working_directory = nullcontext(str(output)) if args.profiler else tempfile.TemporaryDirectory(prefix="woby_e2e_")
        with working_directory as temporary, (output / "console.log").open("w", encoding="utf-8") as console:
            process = subprocess.Popen(command, cwd=temporary, env=environment, startupinfo=startup, stdout=console, stderr=console)
            report["launcher_pid"] = process.pid
            registry_path = Path(os.environ["LOCALAPPDATA"]) / "woby/instances" / ("instance-" + instance + ".json")
            deadline = time.perf_counter() + 90
            while time.perf_counter() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"Viewer launcher exited during startup: {process.returncode}")
                try:
                    registry = json.loads(registry_path.read_text(encoding="utf-8"))
                    if rpc("instance.info").get("ready"):
                        target_pid = registry["pid"]
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.05)
            else:
                raise TimeoutError("Viewer startup timed out")
            report["pid"] = target_pid
            report["startup_ms"] = now()
            watcher.start()
            checkpoint("before_load")
            rpc("performance.begin")
            report["load_request_ms"] = now()
            with ThreadPoolExecutor(max_workers=1) as executor:
                loading = executor.submit(rpc, "model.add", {"path": str(model)}, 600)
                while not loading.done():
                    queried = now()
                    # instance.info reads the server state directly. A queued
                    # status command waits behind the active model.add command.
                    status = rpc("instance.info")
                    report["status"].append({"time_ms": now(), "request_ms": queried, "value": status})
                    if status.get("postLoadError"):
                        raise RuntimeError(status["postLoadError"])
                    if now() - report["load_request_ms"] > 600_000:
                        raise TimeoutError("Model load timed out")
                    time.sleep(.20)
                loaded = loading.result()
            report["load_rpc_ms"] = now() - report["load_request_ms"]
            report["loaded"] = loaded
            if loaded.get("addedCount") != 1 or loaded.get("failedCount"):
                raise RuntimeError(f"Import failed: {loaded}")
            report["ready_status"] = rpc("status")
            report["load_frames"] = rpc("performance.end")
            report["stats"] = rpc("stats")
            report["performance"] = rpc("performance.get")
            checkpoint("after_ready")
            screenshot_begin = now()
            rpc("screenshot.capture", {"path": str(output / "beartrap.png")}, 120)
            report["screenshot_rpc_ms"] = now() - screenshot_begin
            report["load_to_verified_screenshot_ms"] = now() - report["load_request_ms"]
            report["image"] = image_stats(output / "beartrap.png")
            time.sleep(args.settle_seconds)
            checkpoint("after_settle")
            rpc("performance.begin")
            report["render_started_ms"] = now()
            time.sleep(args.render_seconds)
            report["frames"] = rpc("performance.end")
            report["render_duration_ms"] = now() - report["render_started_ms"]
            report["steady_performance"] = rpc("performance.get")
            if args.render_features:
                for helper in ("grid", "origin", "dimensions"):
                    rpc(helper + ".set", {"visible": False})
                variants = (("solid", True, False, False), ("solid_edges", True, True, False),
                            ("solid_points", True, False, True), ("points", False, False, True),
                            ("edges", False, True, False))
                report["feature_captures"] = []
                for name, solid, edges, points in variants:
                    rpc("performance.begin")
                    transition_begin = now()
                    rpc("render.set", {"target": "scene", "solid": solid, "triangles": edges, "vertices": points})
                    time.sleep(2)
                    transition = rpc("performance.end")
                    rpc("performance.begin")
                    begin = now()
                    time.sleep(5)
                    frames = rpc("performance.end")
                    capture = {"name": name, "start_ms": begin, "end_ms": now(),
                               "frames": frames, "performance": rpc("performance.get"),
                               "transition_started_ms": transition_begin, "transition_frames": transition}
                    image_path = output / ("feature-" + name + ".png")
                    rpc("screenshot.capture", {"path": str(image_path)}, 120)
                    capture["image"] = image_stats(image_path)
                    report["feature_captures"].append(capture)
                rpc("render.set", {"target": "scene", "solid": True, "triangles": False, "vertices": False})
            if args.navigation:
                camera = rpc("camera.get")
                report["camera_before_navigation"] = camera
                rpc("performance.begin")
                report["navigation_started_ms"] = now()
                for i in range(24):
                    rpc("camera.orbit", {"yawDegrees": 3.0, "pitchDegrees": 0.5 if i < 12 else -0.5})
                    time.sleep(.1)
                report["navigation_frames"] = rpc("performance.end")
                report["navigation_duration_ms"] = now() - report["navigation_started_ms"]
            if args.memory_map:
                report["remove_result"] = rpc("model.remove", {"target": loaded["addedIds"][0]})
                time.sleep(2); checkpoint("after_remove")
                report["clear_result"] = rpc("scene.new", {"onDirty": "discard"})
                time.sleep(2); checkpoint("after_scene_new")
                time.sleep(10); checkpoint("empty_after_10s")
                report["reload_cycles"] = []
                for cycle in range(2, args.reload_cycles + 1):
                    begin = now()
                    extra = rpc("model.add", {"path": str(model)}, 600)
                    report["reload_cycles"].append({"cycle": cycle, "start_ms": begin, "ready_ms": now(), "loaded": extra})
                    checkpoint(f"cycle_{cycle}_ready")
                    rpc("model.remove", {"target": extra["addedIds"][0]})
                    time.sleep(2); checkpoint(f"cycle_{cycle}_removed")
                    rpc("scene.new", {"onDirty": "discard"})
                    time.sleep(10); checkpoint(f"cycle_{cycle}_empty_after_10s")
            rpc("quit", {"onDirty": "discard"})
            # Full WDDM traces can need several minutes to translate after the
            # target has exited. This time is outside every measured endpoint.
            report["exit_code"] = process.wait(timeout=300 if args.profiler else 45)
            if report["exit_code"]:
                raise RuntimeError(f"Launcher exit code {report['exit_code']}")
    except Exception as error:
        report["error"] = str(error)
        if process and process.poll() is None:
            try:
                if registry:
                    rpc("quit", {"onDirty": "discard"}, 10)
                process.wait(timeout=15)
            except Exception:
                if target_pid:
                    try:
                        psutil.Process(target_pid).terminate()
                    except psutil.Error:
                        pass
                process.terminate(); process.wait(timeout=15)
    finally:
        stopped.set()
        if watcher.ident:
            watcher.join(timeout=5)
        if errors:
            report["observer_errors"] = sorted(set(errors))
        report["trace"] = load_trace(trace_path)
        close_counters(system_counters)
        report["duration_ms"] = now()
        (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    short = {k: report[k] for k in ("executable", "hidden", "profiler", "load_rpc_ms", "load_to_verified_screenshot_ms", "duration_ms", "error") if k in report}
    print(json.dumps(short), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hidden", action="store_true")
    parser.add_argument("--unwarmed", action="store_true")
    parser.add_argument("--profiler", choices=("uprof", "nsys", "nsys-gpu", "nsys-wddm", "nsys-wddm-full", "vtune", "timechart", "ngfx"))
    parser.add_argument("--event-set", choices=("branch", "cache", "fp"), default="branch")
    parser.add_argument("--settle-seconds", type=float, default=5)
    parser.add_argument("--render-seconds", type=float, default=8)
    parser.add_argument("--staging-mib", type=int)
    parser.add_argument("--memory-map", action="store_true")
    parser.add_argument("--navigation", action="store_true")
    parser.add_argument("--render-features", action="store_true")
    parser.add_argument("--reload-cycles", type=int, default=1)
    args = parser.parse_args()
    if min(args.settle_seconds, args.render_seconds) < 0 or max(args.settle_seconds, args.render_seconds) > 120:
        parser.error("Durations must be between zero and 120 seconds")
    report = run(args)
    if "error" in report:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
