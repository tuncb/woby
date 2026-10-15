# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=7"]
# ///
"""Compare packed RGB parsing with generic conversion and an optional old binary."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time

import psutil


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(16 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def quiet():
    samples = []
    deadline = time.monotonic() + 180
    while len(samples) < 2:
        active = [p.info for p in psutil.process_iter(["pid", "name"])
                  if (p.info["name"] or "").lower() in
                  {"cl.exe", "link.exe", "msbuild.exe", "cmake.exe", "ctest.exe", "woby.exe", "vtune.exe"}
                  or (p.info["name"] or "").lower().startswith("woby_")]
        cpu = psutil.cpu_percent(interval=.5)
        if active or cpu > 15:
            samples.clear()
        else:
            samples.append(cpu)
        if time.monotonic() > deadline:
            raise RuntimeError(f"Machine did not become idle: {active}")
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fast", type=Path, required=True)
    parser.add_argument("--generic", type=Path, required=True)
    parser.add_argument("--before", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--synthetic-vertices", type=int, default=2000000)
    parser.add_argument("models", nargs="*", type=Path)
    args = parser.parse_args()
    if args.rounds < 1 or args.synthetic_vertices < 0:
        parser.error("Rounds must be positive; vertex count must be nonnegative.")
    programs = {"fast_rgb": args.fast.resolve(), "generic_rgb": args.generic.resolve()}
    if args.before:
        programs["before_ignored_rgb"] = args.before.resolve()
    root = Path(__file__).resolve().parents[2]
    sources = ["third_party/rapidobj/include/rapidobj/prototype.hpp",
               "third_party/rapidobj/include/rapidobj/rapidobj.hpp",
               "experiments/parser-throughput/benchmark.cpp", "experiments/parser-throughput/generate.py"]
    power = subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip() if platform.system() == "Windows" else None
    report = {"method": "Fresh processes; warm input before each sample; rotating variant order; two idle CPU samples <=15%; no concurrent compilation, tests, or viewer.",
              "rounds": args.rounds, "workers": args.workers, "platform": platform.platform(), "power_scheme": power,
              "physical_cores": psutil.cpu_count(logical=False), "logical_cores": psutil.cpu_count(),
              "ram_bytes": psutil.virtual_memory().total,
              "source_sha256": {name: sha(root / name) for name in sources},
              "programs": {name: {"path": str(path), "sha256": sha(path)} for name, path in programs.items()},
              "inputs": [], "samples": []}
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="woby color benchmark ") as temporary:
        models = [path.resolve() for path in args.models]
        if args.synthetic_vertices:
            path = Path(temporary).resolve() / "decimal-colors.obj"
            block = "".join(f"v {i}.123456 12.500000 -3.125000 0.{i*17%1000000:06d} 0.{i*37%1000000:06d} 0.{i*73%1000000:06d}\n"
                            for i in range(10000)).encode()
            with path.open("wb") as stream:
                for _ in range(args.synthetic_vertices // 10000):
                    stream.write(block)
                for i in range(args.synthetic_vertices % 10000):
                    stream.write(f"v {i} 0 0 0.125000 0.500000 1.000000\n".encode())
            models.append(path)
        for path in models:
            report["inputs"].append({"name": path.name, "bytes": path.stat().st_size, "sha256": sha(path)})
        fingerprints = {}
        for round_index in range(args.rounds):
            for model_index, path in enumerate(models):
                names = list(programs)
                rotation = (round_index + model_index) % len(names)
                for name in names[rotation:] + names[:rotation]:
                    if power and subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip() != power:
                        raise RuntimeError("Power scheme changed during measurements")
                    with path.open("rb") as stream:
                        while stream.read(16 * 1024 * 1024):
                            pass
                    idle = quiet()
                    command = [str(programs[name]), "prototype" if name == "before_ignored_rgb" else "prototype-colors",
                               str(path), "0", str(args.workers)]
                    result = json.loads(subprocess.check_output(command, text=True))
                    for field in ("fingerprint", "precise_fingerprint", "color_fingerprint", "vertex_color_count", "vertex_color_bytes"):
                        if name == "before_ignored_rgb" and field in ("color_fingerprint", "vertex_color_count", "vertex_color_bytes"):
                            continue
                        key = (path.name, field)
                        previous = fingerprints.setdefault(key, result[field])
                        if previous != result[field]:
                            raise RuntimeError(f"{path.name}: {field} differs for {name}")
                    report["samples"].append({"round": round_index, "model": path.name, "variant": name, "idle_cpu_percent": idle, **result})
                    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
                    print(f"{round_index+1}/{args.rounds} {path.name} {name}: {result['total_ms']:.2f} ms", flush=True)
        report["medians"] = {path.name: {name: {field: statistics.median(sample[field] for sample in report["samples"]
            if sample["model"] == path.name and sample["variant"] == name) for field in
            ("total_ms", "cpu_ms", "peak_commit_bytes", "peak_working_set_bytes") if field in report["samples"][-1]}
            for name in programs} for path in models}
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
