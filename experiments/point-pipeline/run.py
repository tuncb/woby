# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil>=6"]
# ///
"""Run the point pipeline experiment with isolated files and resource guards."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time

import psutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("reference_shaders", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--frames", type=int, default=24)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--samples", type=int, choices=(1, 4), default=4)
    parser.add_argument("--zoom", type=float, default=1)
    parser.add_argument("--paired", action="store_true", help="Alternate reference and production at each identical camera position")
    args = parser.parse_args()
    if not 1 <= args.frames <= 120 or not 1 <= args.rounds <= 12 or not math.isfinite(args.zoom) or args.zoom <= 0:
        parser.error("Invalid frame count, round count, or zoom")
    binary, model, reference, output = (p.resolve() for p in
        (args.executable, args.model, args.reference_shaders, args.output))
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("guard", root / "experiments/mesh-overlays/run.py")
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    overlap = guard.overlapping_workloads()
    if overlap:
        raise RuntimeError(f"Another workload is active: {overlap}")
    output.mkdir(parents=True, exist_ok=False)

    def digest(path):
        with path.open("rb") as stream:
            return hashlib.file_digest(stream, "sha256").hexdigest()

    experiment_shaders = binary.parents[2] / "experiments/point-pipeline/shaders"
    report = dict(model=str(model), model_sha256=digest(model), executable=str(binary), binary_sha256=digest(binary),
        reference_shaders={p.name: digest(p) for p in reference.glob("*.bin")},
        experimental_shaders={p.name: digest(p) for p in experiment_shaders.glob("*.bin")},
        production_shaders={p.name: digest(p) for p in (binary.parent / "assets/shaders/spirv").glob("*.bin")},
        source_sha256={str(p.relative_to(root)): digest(p) for p in
            [*Path(__file__).resolve().parent.glob("*"), *root.glob("shaders/native/*")] if p.is_file()},
        source_revision=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip())
    started = time.monotonic()
    peak = 0
    available_min = psutil.virtual_memory().available
    reason = None
    command = [str(binary), str(model), str(reference), str(output / "measurements.json"),
        str(args.frames), str(args.rounds), str(args.samples), str(args.zoom)]
    if args.paired:
        command.append("paired")
    report["command"] = command
    with tempfile.TemporaryDirectory(prefix="woby-point-pipeline-") as directory, (output / "run.log").open("w") as log:
        startup = subprocess.STARTUPINFO() if os.name == "nt" else None
        if startup:
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
        process = subprocess.Popen(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, startupinfo=startup,
            env=dict(os.environ, VK_LOADER_LAYERS_DISABLE="~implicit~"))
        try:
            probe = psutil.Process(process.pid)
            while process.poll() is None:
                try:
                    memory = probe.memory_info()
                    private = getattr(memory, "private", memory.rss)
                except psutil.NoSuchProcess:
                    break
                available = psutil.virtual_memory().available
                peak = max(peak, private)
                available_min = min(available_min, available)
                overlap = guard.overlapping_workloads(process.pid)
                reason = ("overlap" if overlap else "private memory" if private > 38 * 2**30
                    else "available memory" if available < 6 * 2**30
                    else "timeout" if time.monotonic() - started > 1200 else None)
                if reason:
                    process.terminate()
                    break
                time.sleep(.2)
            code = process.wait(timeout=15)
            report.update(exit_code=code, guard_reason=reason, peak_private=peak,
                minimum_available=available_min, elapsed_seconds=time.monotonic() - started)
            (output / "run.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            if reason or code:
                raise RuntimeError(f"Experiment failed: guard={reason}, exit={code}; see {output / 'run.log'}")
            results = json.loads((output / "measurements.json").read_text())
            assert results["exact_samples"] and results["rounds"] and results["validation"]
            print(json.dumps(dict(status="completed", cases=len(results["validation"]),
                samples=sum(row["samples"] for row in results["validation"]), elapsed_seconds=report["elapsed_seconds"],
                binary_sha256=report["binary_sha256"], output=str(output))), flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)


if __name__ == "__main__":
    main()
