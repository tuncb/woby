# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Measure the current BearTrap pipeline and parser controls in fresh processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

import psutil

ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / "experiments/obj-profile"
sys.path.insert(0, str(PROFILE))
from e2e_batch import schedule
from e2e_run import digest


def write(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def quiet():
    samples = [psutil.cpu_percent(interval=.25) for _ in range(3)]
    return samples


def launch(command, directory, timeout=900):
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "driver.log").open("w", encoding="utf-8") as output:
        completed = subprocess.run([str(v) for v in command], stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
    if completed.returncode:
        raise RuntimeError(f"Capture failed: {directory}; see driver.log")


def native(args):
    output = args.output / "native"
    output.mkdir(exist_ok=True)
    manifest = {"method": "Sequential fresh processes; rotate configuration order; warm input before every launch; no profiler", "runs": []}
    for ordinal, (round_number, configuration) in enumerate(schedule(
            ["production-visible", "traced-visible", "traced-hidden", "production-hidden"], args.rounds), 1):
        prefix, visibility = configuration.split("-")
        directory = output / f"{ordinal:03d}-r{round_number}-{configuration}"
        exe = args.bin / ("woby.exe" if prefix == "production" else "woby_obj_e2e_viewer.exe")
        row = {"round": round_number, "configuration": configuration, "output": str(directory), "background_cpu_before": quiet()}
        print(f"Native {ordinal}/{4 * args.rounds}: {configuration}", flush=True)
        if not (directory / "result.json").exists():
            command = [sys.executable, PROFILE / "e2e_run.py", exe, args.model, "--output", directory,
                       "--settle-seconds", "1", "--render-seconds", "2"]
            if visibility == "hidden":
                command.append("--hidden")
            launch(command, directory)
        record = json.loads((directory / "result.json").read_text())
        if record.get("error") or record.get("exit_code") != 0:
            raise RuntimeError(f"Incomplete native capture: {directory}")
        row.update(load_rpc_ms=record["load_rpc_ms"], executable_sha256=record["executable_sha256"])
        manifest["runs"].append(row)
        write(output / "manifest.json", manifest)
        print(f"  Ready RPC: {record['load_rpc_ms'] / 1000:.3f} s", flush=True)


def parser(args):
    configurations = [(f"file-w{w}", "woby_parser_benchmark.exe", "prototype", 0, w) for w in (1, 2, 4, 8, 16, 32)]
    configurations += [(f"file-block-{m}MiB", "woby_parser_benchmark.exe", "prototype", m * 1024**2, 16) for m in (1, 2, 8, 16)]
    configurations += [(f"modern-w{w}", "woby_parser_benchmark_modern.exe", "prototype", 0, w) for w in (1, 8, 16)]
    configurations += [("memory-w16", "woby_parser_benchmark.exe", "prototype-memory", 0, 16),
                       ("modern-memory-w16", "woby_parser_benchmark_modern.exe", "prototype-memory", 0, 16),
                       ("legacy-w16", "woby_parser_benchmark.exe", "legacy", 0, 16)]
    output = args.output / "parser"
    output.mkdir(exist_ok=True)
    report = {"method": "Fresh process; rotated configuration order; warmed input; ordered output fingerprint outside the parse timer", "records": []}
    path = output / "results.json"
    saved = json.loads(path.read_text()) if path.exists() else {"records": []}
    records = {(row["round"], row["configuration"]): row for row in saved["records"]}
    fingerprint = None
    for round_index in range(args.rounds):
        order = configurations[round_index % len(configurations):] + configurations[:round_index % len(configurations)]
        for label, name, mode, block, workers in order:
            print(f"Parser round {round_index + 1}: {label}", flush=True)
            row = records.get((round_index + 1, label))
            if row is None:
                with args.model.open("rb") as source:
                    for _ in iter(lambda: source.read(8 * 1024**2), b""):
                        pass
                background = quiet()
                command = [str(args.bin / name), mode, str(args.model), str(block), str(workers)]
                start = time.perf_counter()
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=300, check=True)
                row = json.loads(result.stdout)
                row.update(round=round_index + 1, configuration=label, command=command, background_cpu_before=background,
                           fresh_process_wall_ms=(time.perf_counter() - start) * 1000)
            if fingerprint is None:
                fingerprint = row["fingerprint"]
            if row["fingerprint"] != fingerprint:
                raise RuntimeError(f"Output differs for {label}: {row['fingerprint']} != {fingerprint}")
            report["records"].append(row)
            write(path, report)
            print(f"  Parse {row['total_ms'] / 1000:.3f} s; {row['average_logical_cpus']:.2f} logical CPUs; fingerprint matches", flush=True)


def controls(args):
    output = args.output / "controls"
    output.mkdir(exist_ok=True)
    path = output / "results.json"
    records = json.loads(path.read_text()) if path.exists() else []
    cases = [(mode, w) for mode in ("read-cached", "read-native") for w in (1, 8, 16)] + [("memory", 0)]
    for round_index in range(args.rounds):
        for mode, workers in cases[round_index:] + cases[:round_index]:
            if any(r["round"] == round_index + 1 and r["mode"] == mode and r["workers_requested"] == workers for r in records):
                continue
            print(f"Control round {round_index + 1}: {mode}, workers {workers}", flush=True)
            with args.model.open("rb") as source:
                for _ in iter(lambda: source.read(8 * 1024**2), b""):
                    pass
            command = [str(args.bin / "woby_obj_profile_avx2.exe"), mode, str(args.model), str(workers)]
            completed = subprocess.run(command, capture_output=True, text=True, check=True, timeout=180)
            record = json.loads(completed.stdout)
            record.update(round=round_index + 1, workers_requested=workers, command=command)
            records.append(record)
            write(path, records)


def deep(args):
    launch([sys.executable, PROFILE / "e2e_deep.py", args.bin, args.model, "--output", args.output / "deep",
            "--cases", "branch,cache,fp,timechart,nsys-wddm-retry,vtune"], args.output / "deep", timeout=1800)
    print("Deep CPU, power, and GPU captures completed", flush=True)


def hardware(args):
    from deep_tools import UPROF, summarize
    from e2e_hardware import caller_counts
    for case in ("branch", "cache", "fp"):
        output = args.output / "deep" / case
        session = output / "cpu"
        symbols = session / "matching-symbols"
        symbols.mkdir(exist_ok=True)
        for name in ("woby_obj_e2e_viewer.exe", "woby_obj_e2e_viewer.pdb"):
            shutil.copy2(args.output / "matching-bin" / name, symbols / name)
        print("Translate hardware capture: " + case, flush=True)
        with (output / "translation.log").open("w", encoding="utf-8") as log:
            subprocess.run([str(UPROF), "report", "-i", str(session), "--cutoff", "0", "--symbol-path", str(symbols)],
                           stdout=log, stderr=subprocess.STDOUT, timeout=900, check=True)
        viewer = json.loads((output / "result.json").read_text())
        metadata = next(t for t in viewer["trace"] if t["kind"] == "metadata")
        probe = {"mode": "viewer", "compiler_isa": "AVX2", "clock_origin": metadata,
                 "events": [t for t in viewer["trace"] if t["kind"] == "phase"]}
        analysis = caller_counts(session, viewer, summarize(session, output, probe))
        write(output / "cpu-analysis.json", analysis)
    launch([sys.executable, PROFILE / "e2e_nsys.py", args.output], args.output / "gpu-analysis", timeout=900)
    vtune = Path(r"C:\Program Files (x86)\Intel\oneAPI\vtune\2025.3\bin64\vtune.exe")
    for grouping in ("function", "thread"):
        output = args.output / "deep/vtune"
        with (output / f"hotspots-{grouping}.csv").open("w", encoding="utf-8") as stream:
            subprocess.run([str(vtune), "-report", "hotspots", "-r", str(output / "vtune"), "-format", "csv", "-group-by", grouping],
                           stdout=stream, stderr=subprocess.STDOUT, timeout=600, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("bin", type=Path)
    ap.add_argument("model", type=Path)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--fast-float-header", type=Path, help="Standalone research header used to build the modern decoder target")
    ap.add_argument("--steps", default="native,parser,controls,deep,hardware")
    args = ap.parse_args()
    args.bin = args.bin.resolve(strict=True); args.model = args.model.resolve(strict=True); args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if not (args.output / "provenance.json").exists():
        symbols = args.output / "matching-bin"
        symbols.mkdir(exist_ok=True)
        executables = {}
        for name in ("woby", "woby_obj_e2e_viewer", "woby_obj_profile_avx2", "woby_parser_benchmark", "woby_parser_benchmark_modern"):
            for suffix in (".exe", ".pdb"):
                source = args.bin / (name + suffix)
                if source.exists():
                    shutil.copy2(source, symbols / (name + suffix))
            executables[name] = digest(symbols / (name + ".exe"))
        for path in (ROOT / "build/vs2026-vcpkg/experiments/obj-profile").glob("*.map"):
            shutil.copy2(path, symbols / path.name)
        source_paths = list((ROOT / "src").glob("*.cpp")) + list((ROOT / "src").glob("*.h"))
        source_paths += list((ROOT / "third_party/rapidobj/include/rapidobj").glob("*.hpp"))
        write(args.output / "provenance.json", {"revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            "model": str(args.model), "model_bytes": args.model.stat().st_size, "model_sha256": digest(args.model),
            "os": platform.platform(), "logical_cpus": psutil.cpu_count(), "physical_cpus": psutil.cpu_count(logical=False),
            "ram_bytes": psutil.virtual_memory().total, "initial_available_memory": psutil.virtual_memory().available,
            "captured_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "executable_sha256": executables,
            "source_sha256": {str(path.relative_to(ROOT)): digest(path) for path in source_paths},
            "fast_float_v8_3_2_sha256": digest(args.fast_float_header.resolve(strict=True) if args.fast_float_header else args.output / "fast_float-v8.3.2.h")})
    for name in args.steps.split(","):
        if name:
            globals()[name](args)


if __name__ == "__main__":
    main()
