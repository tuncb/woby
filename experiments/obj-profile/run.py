# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6"]
# ///
"""Run fresh processes sequentially. Keep results after every completed run."""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import time
from pathlib import Path

import psutil


def warm(path):
    with path.open("rb") as source:
        while source.read(8 * 1024 * 1024):
            pass


def measure(exe, mode, model, workers, instructions, timeout, reserve=0):
    command = [str(exe), mode, str(model), str(workers)]
    if instructions:
        command.append("sample")
    start = time.perf_counter()
    before_cpu = psutil.cpu_times(percpu=True)
    before_disk = psutil.disk_io_counters(perdisk=True)
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                               env=dict(os.environ, WOBY_PROFILE_RESERVE_PERCENT=str(reserve)))
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        if process.returncode:
            raise RuntimeError(f"Probe failed ({process.returncode}): {stderr}")
    except BaseException:
        process.kill()
        process.communicate()
        raise
    finish = time.perf_counter()
    output = json.loads(stdout)
    output["process_wall_ms"] = (finish - start) * 1000
    output["system_cpu_deltas"] = [
        {field: getattr(after, field) - getattr(before, field) for field in before._fields}
        for before, after in zip(before_cpu, psutil.cpu_times(percpu=True))]
    output["system_disk_deltas"] = {
        disk: {field: getattr(after, field) - getattr(before_disk[disk], field) for field in after._fields}
        for disk, after in psutil.disk_io_counters(perdisk=True).items() if disk in before_disk}
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("--modes", default="parser")
    parser.add_argument("--workers", default="1,2,4,8,16")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--reserve-percent", default="0")
    parser.add_argument("--instructions", action="store_true")
    parser.add_argument("--compare-executable", type=Path)
    parser.add_argument("--timeout", type=float, default=1200)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("rounds must be positive")
    exe, model = args.executable.resolve(strict=True), args.model.resolve(strict=True)
    with model.open("rb") as source:
        model_digest = hashlib.file_digest(source, "sha256").hexdigest()
    executables = [exe]
    if args.compare_executable:
        executables.append(args.compare_executable.resolve(strict=True))
    configurations = [(program, mode, int(workers), int(reserve)) for program in executables
                      for mode in args.modes.split(",") for workers in args.workers.split(",")
                      for reserve in args.reserve_percent.split(",")]
    output = {"method": "Fresh Release processes; sequential runs; file warmed before each process; rotating configuration order",
              "executable": str(exe), "model": str(model), "file_bytes": model.stat().st_size, "file_sha256": model_digest,
              "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
              "os": platform.platform(), "memory_bytes": psutil.virtual_memory().total,
              "initial_available_memory": psutil.virtual_memory().available,
              "executable_sha256": {str(program): hashlib.sha256(program.read_bytes()).hexdigest() for program in executables},
              "probe_source_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in Path(__file__).parent.iterdir() if path.is_file()}, "records": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for round_index in range(args.rounds):
        rotation = round_index % len(configurations)
        order = configurations[rotation:] + configurations[:rotation]
        for program, mode, workers, reserve in order:
            warm(model)
            record = measure(program, mode, model, workers, args.instructions, args.timeout, reserve)
            record.update(round=round_index + 1, executable=program.name, reserve_percent=reserve)
            output["records"].append(record)
            args.output.write_text(json.dumps(output, indent=2), encoding="utf-8")
            stages = {row["name"]: row for row in record["events"]}
            total = stages.get("cpu_import", stages.get("parser_total", stages.get("read_only", stages.get("memory_copy"))))
            print(json.dumps({"mode": mode, "isa": record["compiler_isa"], "workers": workers, "round": round_index + 1,
                              "wall_ms": total["wall_ms"], "average_logical_cpus": total["average_logical_cpus"],
                              "peak_GiB": total["process_peak_working_set"] / 2**30}), flush=True)
    for mode in args.modes.split(","):
        records = [record for record in output["records"] if record["mode"] == mode]
        geometries = [{key: value for key, value in record["geometry"].items() if key not in ("normal_fingerprint", "retained_buffer_capacity_bytes")}
                      for record in records if "geometry" in record]
        if geometries and any(value != geometries[0] for value in geometries):
            raise RuntimeError(f"Geometry mismatch in {mode}")


if __name__ == "__main__":
    main()
