# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6"]
# ///
"""Run one fixed-resolution overlay experiment with the stress campaign guards."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

import psutil


def overlapping_workloads(excluded_pid=None):
    names = {"woby.exe", "woby_tests.exe", "woby_overlay_tests.exe",
             "woby_overlay_prototype.exe", "woby_point_prototype.exe", "woby_nographicsapi_prototype.exe",
             "ctest.exe", "msbuild.exe", "cl.exe", "link.exe", "slangc.exe"}
    result = []
    for process in psutil.process_iter(["pid", "name", "cmdline"]):
        name = (process.info["name"] or "").lower()
        if process.info["pid"] == excluded_pid or name not in names:
            continue
        # MSBuild keeps idle worker nodes alive after a completed build. The
        # active coordinator and its compiler/linker children remain guarded.
        if name == "msbuild.exe" and any("/nodemode:" in arg.lower()
                                         for arg in process.info["cmdline"] or []):
            continue
        result.append(dict(pid=process.info["pid"], name=process.info["name"]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--private-gib", type=float, default=38)
    parser.add_argument("--available-gib", type=float, default=6)
    parser.add_argument("--timeout", type=float, default=1200)
    args, options = parser.parse_known_args()
    destination = args.output.resolve()
    if destination.exists():
        parser.error("Use a new output directory")
    if any(not math.isfinite(value) or value <= 0 for value in (args.private_gib, args.available_gib, args.timeout)):
        parser.error("Resource guard limits must be positive")
    destination.mkdir(parents=True)
    repository = Path(__file__).resolve().parents[2]
    source_paths = list(Path(__file__).resolve().parent.glob("*")) + [
        repository / "shaders/native/woby.slang", repository / "shaders/native/root.h",
        repository / "src/scene_mesh_preparation.cpp", repository / "src/obj_mesh.cpp",
        repository / "src/model_mesh.cpp", repository / "src/camera.cpp"]
    source_hashes = {str(path.relative_to(repository)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in source_paths if path.is_file()}
    shader_directory = args.executable.resolve().parents[2] / "experiments/mesh-overlays/shaders"
    shader_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in shader_directory.glob("*.spv")}
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repository,
                              capture_output=True, text=True, check=True).stdout.strip()
    with args.model.open("rb") as source:
        model_hash = hashlib.file_digest(source, "sha256").hexdigest()
    overlap = overlapping_workloads()
    if overlap:
        parser.error(f"Another Woby workload is active; wait and use a new output directory: {overlap}")
    command = [str(args.executable.resolve()), "--model", str(args.model.resolve()),
               "--output", str(destination / "captures"), *options]
    env = dict(os.environ, VK_LOADER_LAYERS_DISABLE="~implicit~")
    start = time.monotonic()
    reason = "completed"
    peak_private = 0
    minimum_available = psutil.virtual_memory().available
    with (destination / "viewer.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env)
        probe = psutil.Process(process.pid)
        try:
            while process.poll() is None:
                try:
                    memory = probe.memory_info()
                    private = getattr(memory, "private", memory.rss)
                except psutil.NoSuchProcess:
                    break
                available = psutil.virtual_memory().available
                peak_private = max(peak_private, private)
                minimum_available = min(minimum_available, available)
                overlap = overlapping_workloads(process.pid)
                if overlap:
                    reason = "concurrent_woby_workload"
                elif private > args.private_gib * 2**30:
                    reason = "private_memory_guard"
                elif available < args.available_gib * 2**30:
                    reason = "available_memory_guard"
                elif time.monotonic() - start > args.timeout:
                    reason = "timeout"
                if reason != "completed":
                    process.terminate()
                    break
                time.sleep(.2)
            code = process.wait(timeout=10)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
    report = dict(command=command, status=reason, exit_code=code,
                  elapsed_seconds=time.monotonic()-start,
                  peak_private_bytes=peak_private, minimum_available_bytes=minimum_available,
                  sample_interval_seconds=.2, private_limit_gib=args.private_gib,
                  available_limit_gib=args.available_gib, overlapping_workloads=overlap, source_revision=revision,
                  source_sha256=source_hashes, model_sha256=model_hash,
                  shader_sha256=shader_hashes,
                  executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest())
    (destination / "guard.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if reason == "completed" and code == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
