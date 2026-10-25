# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil>=6"]
# ///
"""Run paired shader variants with exact sample checks and resource guards."""
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

def workloads(values):
    result = []
    for value in values:
        kind, _, size = value.partition(":")
        if kind not in ("solid", "edges", "pulled", "lines", "uv", "comparison", "quads", "points", "combined",
                        "line-sprites", "transparent", "transparent-edges", "transparent-pulled"):
            raise ValueError(f"Unknown workload: {kind}")
        diameter = float(size or 4)
        if not math.isfinite(diameter) or not 1 <= diameter <= 40:
            raise ValueError("Marker diameter must be 1..40")
        workload = dict(kind=kind, size=diameter)
        if workload in result: raise ValueError("Duplicate workload")
        result.append(workload)
    return result


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    import psutil
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", help="Absolute OBJ path, or fixture")
    parser.add_argument("reference", type=Path)
    parser.add_argument("variant_root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--variants", nargs="+", required=True)
    parser.add_argument("--workloads", nargs="+", default=["solid", "edges", "combined:1", "combined:4", "combined:8"])
    parser.add_argument("--frames", type=int, default=12)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--samples", type=int, choices=(1,4), default=4)
    parser.add_argument("--zoom", type=float, default=1)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=687)
    parser.add_argument("--allow-mismatch", action="store_true", help="Record rejected candidates and finish screening")
    args = parser.parse_args()
    binary, reference, variants, output = (p.resolve() for p in (args.executable, args.reference, args.variant_root, args.output))
    model = "fixture" if args.model == "fixture" else str(Path(args.model).resolve())
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("guard", root/"experiments/mesh-overlays/run.py")
    guard = importlib.util.module_from_spec(spec); spec.loader.exec_module(guard)
    if not 1 <= args.frames <= 120 or not 1 <= args.rounds <= 12 or not math.isfinite(args.zoom) or args.zoom <= 0:
        parser.error("Invalid frames, rounds, or zoom")
    if not 1 <= args.width <= 3840 or not 1 <= args.height <= 2160: parser.error("Target must fit 3840 by 2160")
    if "reference" in args.variants or len(set(args.variants)) != len(args.variants):
        parser.error("Variant names must be unique and must not include reference")
    config = dict(model=model, frames=args.frames, rounds=args.rounds, samples=args.samples, zoom=args.zoom,
        width=args.width, height=args.height,
        allow_mismatch=args.allow_mismatch, workloads=workloads(args.workloads),
        variants=[dict(name="reference", directory=str(reference))] +
            [dict(name=name, directory=str(reference if name=="repeat" else variants/name/"spirv")) for name in args.variants])
    overlap = guard.overlapping_workloads()
    if overlap: raise RuntimeError(f"Another workload is active: {overlap}")
    output.mkdir(parents=True, exist_ok=False)
    config_path = output/"configuration.json"
    config_path.write_text(json.dumps(config, indent=2))
    report = dict(configuration=config, executable_sha256=digest(binary),
        model_sha256=None if model=="fixture" else digest(Path(model)),
        revision=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        shader_sha256={v["name"]:{p.name:digest(p) for p in Path(v["directory"]).glob("*.bin")} for v in config["variants"]},
        source_sha256={str(p.relative_to(root)):digest(p) for p in
            [*Path(__file__).resolve().parent.glob("*"), *root.glob("shaders/native/*")] if p.is_file()})
    command = [str(binary), "--audit", str(config_path), str(output/"measurements.json")]
    report["command"] = command
    started = time.monotonic(); peak = 0; available_min = psutil.virtual_memory().available; reason = None
    with tempfile.TemporaryDirectory(prefix="woby-shader-audit-") as temporary, (output/"run.log").open("w") as log:
        startup = subprocess.STARTUPINFO() if os.name == "nt" else None
        if startup:
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
        process = subprocess.Popen(command, cwd=temporary, stdout=log, stderr=subprocess.STDOUT, startupinfo=startup,
            env=dict(os.environ, VK_LOADER_LAYERS_DISABLE="~implicit~"))
        try:
            probe = psutil.Process(process.pid)
            while process.poll() is None:
                try:
                    memory = probe.memory_info(); private = getattr(memory, "private", memory.rss)
                except psutil.NoSuchProcess: break
                available = psutil.virtual_memory().available
                peak = max(peak, private); available_min = min(available_min, available)
                overlap = guard.overlapping_workloads(process.pid)
                reason = ("overlap" if overlap else "private memory" if private>38*2**30 else
                    "available memory" if available<6*2**30 else "timeout" if time.monotonic()-started>1200 else None)
                if reason:
                    process.terminate(); break
                time.sleep(.2)
            code = process.wait(timeout=15)
            report.update(exit_code=code, guard_reason=reason, peak_private=peak, minimum_available=available_min,
                elapsed_seconds=time.monotonic()-started)
            (output/"run.json").write_text(json.dumps(report, indent=2))
            if reason or code: raise RuntimeError(f"Audit failed: guard={reason}, exit={code}; see {output/'run.log'}")
            data = json.loads((output/"measurements.json").read_text())
            print(json.dumps(dict(output=str(output), exact=data["exact_samples"], cases=len(data["validation"]),
                elapsed_seconds=report["elapsed_seconds"])), flush=True)
        finally:
            if process.poll() is None: process.kill(); process.wait(timeout=15)


if __name__ == "__main__":
    main()
