# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12"]
# ///
"""Check the production viewer with a real import, GPU upload, and readback."""
import argparse
from contextlib import nullcontext
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
from memory_map import read_process_map


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--render-seconds", type=float, default=8.0)
    parser.add_argument("--profiler", choices=("nsys", "ngfx", "uprof"))
    parser.add_argument("--profiler-path", type=Path)
    parser.add_argument("--settle-seconds", type=float, default=1.0)
    parser.add_argument("--memory-map", action="store_true",
                        help="Capture address maps and page validity, then remove the model and reset the scene before quitting")
    args = parser.parse_args()
    if not 0 <= args.render_seconds <= 60:
        parser.error("--render-seconds must be between zero and 60")
    if not 0 <= args.settle_seconds <= 120:
        parser.error("--settle-seconds must be between zero and 120")
    if args.memory_map and args.profiler:
        parser.error("--memory-map uses a native viewer without a profiler wrapper")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    exe, model = args.executable.resolve(strict=True), args.model.resolve(strict=True)
    instance = "obj-profile-" + uuid.uuid4().hex[:12]
    command = [str(exe), "--instance", instance, "--hidden-window", "--drawable-size", "1280x720",
               "--log-file", str(args.output / "viewer.log"), "--log-level", "info"]
    if args.profiler:
        if not args.profiler_path:
            parser.error("--profiler requires --profiler-path")
        profiler = str(args.profiler_path.resolve(strict=True))
        if args.profiler == "nsys":
            command = [profiler, "profile", "--trace=vulkan", "--sample=none", "--cpuctxsw=none",
                       "--vulkan-gpu-workload=individual", "--wait=all", "--export=sqlite",
                       "--output=" + str(args.output / "vulkan")] + command
        elif args.profiler == "ngfx":
            command = [profiler, "--activity=GPU Trace Profiler", "--platform=Windows (x86_64)",
                       "--exe=" + str(exe), "--args=" + subprocess.list2cmdline(command[1:]),
                       "--start-after-ms=3000", "--max-duration-ms=500", "--auto-export",
                       "--output-dir=" + str(args.output)]
        else:
            command = [profiler, "collect", "-e", "event=pmcx076,interval=1000000,user=1,os=0",
                       "-e", "event=pmcx0c0,interval=1000000,user=1,os=0",
                       "-e", "event=pmcx0c3,interval=25000,user=1,os=0",
                       "-o", str(args.output / "cpu")] + command
    report = {"method": "Fresh hidden viewer; cached file; RPC model.add followed by a screenshot readback",
              "executable": str(exe), "settle_seconds": args.settle_seconds,
              "events": [], "samples": []}
    gpu = None
    try:
        pynvml.nvmlInit()
        gpu = pynvml.nvmlDeviceGetHandleByIndex(0)
        report["gpu"] = {"name": pynvml.nvmlDeviceGetName(gpu),
                         "memory_total": pynvml.nvmlDeviceGetMemoryInfo(gpu).total,
                         "scope": "Device-wide NVML values; includes other applications", "samples": []}
    except pynvml.NVMLError as error:
        report["gpu_error"] = str(error)
    with model.open("rb") as source:
        while source.read(8 * 1024 * 1024):
            pass
    working_directory = nullcontext(str(args.output)) if args.profiler else tempfile.TemporaryDirectory(prefix="woby_obj_viewer_")
    with working_directory as temporary, (args.output / "console.log").open("w", encoding="utf-8") as log:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        process = subprocess.Popen(command, cwd=temporary, stdout=log, stderr=log, startupinfo=startup,
                                   env=dict(os.environ, VK_LOADER_LAYERS_DISABLE="~implicit~",
                                            VK_LOADER_LAYERS_ALLOW="VK_LAYER_NV_nsight*,VK_LAYER_NV_GPU_Trace*,VK_LAYER_NV_nomad*" if args.profiler else ""))
        started = time.perf_counter()
        report["observer_epoch_qpc_ns"] = int(started * 1e9)
        stopped = threading.Event()
        def observe():
            probe = psutil.Process(process.pid)
            while not stopped.wait(.05) and process.poll() is None:
                try:
                    if args.profiler and probe.pid == process.pid:
                        candidates = [child for child in probe.children(recursive=True)
                                      if child.name().lower() == exe.name.lower()]
                        if candidates:
                            probe = candidates[0]
                    memory = probe.memory_info()
                    cpu = probe.cpu_times()
                    report["samples"].append({"time_ms": (time.perf_counter() - started) * 1000,
                                              "cpu_ms": (cpu.user + cpu.system) * 1000,
                                              "working_set": memory.rss, "private_bytes": memory.private,
                                              "threads": probe.num_threads()})
                    if gpu and len(report["samples"]) % 4 == 0:
                        try:
                            usage = pynvml.nvmlDeviceGetUtilizationRates(gpu)
                            report["gpu"]["samples"].append({"time_ms": (time.perf_counter() - started) * 1000,
                                "gpu_percent": usage.gpu, "memory_percent": usage.memory,
                                "memory_used": pynvml.nvmlDeviceGetMemoryInfo(gpu).used,
                                "sm_clock_MHz": pynvml.nvmlDeviceGetClockInfo(gpu, pynvml.NVML_CLOCK_SM),
                                "temperature_C": pynvml.nvmlDeviceGetTemperature(gpu, pynvml.NVML_TEMPERATURE_GPU),
                                "power_W": pynvml.nvmlDeviceGetPowerUsage(gpu) / 1000})
                        except pynvml.NVMLError:
                            pass
                except psutil.NoSuchProcess:
                    return
        watcher = threading.Thread(target=observe, daemon=True)
        watcher.start()
        request_id = 0
        registry = {}
        def memory_snapshot(name):
            if not args.memory_map:
                return
            snapshot = read_process_map(process.pid, resident=True)
            memory = psutil.Process(process.pid).memory_info()
            snapshot.update(name=name, time_ms=(time.perf_counter()-started)*1000,
                            private_bytes=memory.private, working_set_bytes=memory.rss)
            report.setdefault("memory_maps", []).append(snapshot)

        def rpc(method, params=None, timeout=60):
            nonlocal request_id
            request_id += 1
            payload = dict(params or {})
            if method not in ("instance.info", "command.get"):
                payload["timeoutSeconds"] = timeout
            request = urllib.request.Request(f"http://127.0.0.1:{registry['port']}/rpc",
                data=json.dumps({"jsonrpc": "2.0", "id": request_id, "method": method, "params": payload}).encode(),
                headers={"Content-Type": "application/json", "Authorization": "Bearer " + registry["token"]})
            with urllib.request.urlopen(request, timeout=timeout+5) as response:
                value = json.load(response)
            if "error" in value:
                raise RuntimeError(f"{method}: {value['error']}")
            return value["result"]
        try:
            registry_path = Path(os.environ["LOCALAPPDATA"]) / "woby/instances" / ("instance-" + instance + ".json")
            deadline = time.perf_counter() + 45
            while time.perf_counter() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("Viewer exited during startup")
                try:
                    registry = json.loads(registry_path.read_text(encoding="utf-8"))
                    if rpc("instance.info").get("ready"):
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.05)
            else:
                raise TimeoutError("Viewer startup timed out")
            report["startup_ms"] = (time.perf_counter() - started) * 1000
            memory_snapshot("before_load")
            begin = time.perf_counter()
            report["load_request_ms"] = (begin - started) * 1000
            loaded = rpc("model.add", {"path": str(model)}, 600)
            report["load_rpc_ms"] = (time.perf_counter() - begin) * 1000
            if loaded["addedCount"] != 1 or loaded["failedCount"] != 0:
                raise RuntimeError(f"Import failed: {loaded}")
            memory_snapshot("after_load")
            report["stats"] = rpc("stats")
            report["performance"] = rpc("performance.get")
            rpc("performance.begin")
            screenshot_start = time.perf_counter()
            rpc("screenshot.capture", {"path": str(args.output / "beartrap.png")}, 120)
            report["load_to_verified_screenshot_ms"] = (time.perf_counter() - begin) * 1000
            report["screenshot_rpc_ms"] = (time.perf_counter() - screenshot_start) * 1000
            report["screenshot_frames"] = rpc("performance.end")
            # Separate ordinary frames from screenshot allocation and readback.
            time.sleep(args.settle_seconds)
            memory_snapshot("after_settle")
            rpc("performance.begin")
            render_begin = time.perf_counter()
            report["render_started_ms"] = (render_begin - started) * 1000
            time.sleep(args.render_seconds)
            report["frames"] = rpc("performance.end")
            report["render_duration_ms"] = (time.perf_counter() - render_begin) * 1000
            report["steady_performance"] = rpc("performance.get")
            if args.memory_map:
                report["remove_result"] = rpc("model.remove", {"target": loaded["addedIds"][0]})
                time.sleep(2)
                memory_snapshot("after_remove")
                report["empty_stats"] = rpc("stats")
                report["clear_result"] = rpc("scene.new", {"onDirty": "discard"})
                time.sleep(2)
                memory_snapshot("after_clear_history")
                time.sleep(8)
                memory_snapshot("empty_after_10s")
            rpc("quit", {"onDirty": "discard"})
            if process.wait(timeout=30) != 0:
                raise RuntimeError("Viewer did not exit cleanly")
        finally:
            stopped.set()
            watcher.join(timeout=2)
            if process.poll() is None:
                process.kill()
                process.wait()
            if gpu and report["gpu"]["samples"]:
                readings = report["gpu"]["samples"]
                if all(reading["memory_percent"] == 100 for reading in readings):
                    report["gpu"]["memory_percent_note"] = (
                        "NVML returned a constant 100% on this WDDM system. "
                        "These readings are not used as a memory bandwidth measurement.")
            (args.output / "viewer.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if gpu:
        pynvml.nvmlShutdown()
    print(json.dumps({key: value for key, value in report.items() if key not in ("samples", "performance", "stats", "frames", "gpu", "memory_maps")}, indent=2))


if __name__ == "__main__":
    main()
