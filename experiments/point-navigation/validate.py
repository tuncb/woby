# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil>=6"]
# ///
"""Compare full-source GPU sample winners with a retained baseline shader."""
import argparse
import hashlib
import importlib.util
import json
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
    parser.add_argument("baseline_shader", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    binary, model, baseline, output = (p.resolve() for p in
        (args.executable, args.model, args.baseline_shader, args.output))
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("guard", root / "experiments/mesh-overlays/run.py")
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    overlap = guard.overlapping_workloads()
    if overlap:
        raise RuntimeError(f"Another workload is active: {overlap}")
    output.mkdir(parents=True, exist_ok=False)
    def digest(path):
        with path.open("rb") as source:
            return hashlib.file_digest(source, "sha256").hexdigest()
    report = dict(model=str(model), model_sha256=digest(model), binary_sha256=digest(binary),
        baseline_shader_sha256=digest(baseline),
        shaders={p.name:digest(p) for p in (binary.parent / "assets/shaders/spirv").glob("*opaque*.bin")},
        source_revision=subprocess.check_output(["git","rev-parse","HEAD"], cwd=root, text=True).strip())
    started = time.monotonic()
    peak = 0
    available_min = psutil.virtual_memory().available
    reason = None
    with tempfile.TemporaryDirectory(prefix="woby-point-validation-") as directory, (output / "validation.log").open("w") as log:
        startup = subprocess.STARTUPINFO() if os.name=="nt" else None
        if startup:
            startup.dwFlags=subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow=0
        process = subprocess.Popen([str(binary), str(model), str(baseline), str(output / "samples.json")],
            cwd=directory, stdout=log, stderr=subprocess.STDOUT, startupinfo=startup,
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
                reason = ("overlap" if overlap else "private memory" if private>38*2**30
                    else "available memory" if available<6*2**30
                    else "timeout" if time.monotonic()-started>1200 else None)
                if reason:
                    process.terminate()
                    break
                time.sleep(.2)
            code = process.wait(timeout=15)
            report.update(exit_code=code, guard_reason=reason, peak_private=peak,
                minimum_available=available_min, elapsed_seconds=time.monotonic()-started)
            (output / "run.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            if reason or code:
                raise RuntimeError(f"Validation failed: guard={reason}, exit={code}; see {output / 'validation.log'}")
            rows = json.loads((output / "samples.json").read_text())["cases"]
            assert len(rows)==24 and all(r["different_depth_id_pairs"]==0 for r in rows)
            print(json.dumps(dict(cases=len(rows), compared_samples=sum(r["compared_samples"] for r in rows),
                different_depth_id_pairs=0, **report)), flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)


if __name__=="__main__":
    main()
