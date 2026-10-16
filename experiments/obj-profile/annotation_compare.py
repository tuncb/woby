# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Compare normal generation, import, preparation and viewer loads in fresh processes."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

import psutil


def competitors(owned=()):
    result = []
    for process in psutil.process_iter(["pid", "name"]):
        name = (process.info["name"] or "").lower()
        if process.pid not in owned and (name in {"cl.exe", "link.exe", "slangc.exe", "ctest.exe"}
                                        or name.startswith("woby")):
            result.append({"pid": process.pid, "name": name})
    return result


def sample_metrics(mode, result):
    if mode == "import":
        return {"wall_ms": result["cpu_load_ms"], "peak_commit_bytes": result["peak_commit_bytes"],
                "output": {key: result[key] for key in ("fingerprint", "color_fingerprint")}}
    measured = dict(result["records"][0])
    if mode == "normals":
        measured["output"] = {"normal_digest": measured["normal_digest"],
                              "vertices": result["vertices"], "triangles": result["triangles"]}
    return measured


def viewer_contention(contention, result):
    # A child can start between the process-tree read and the competitor scan.
    # The completed capture supplies its identity even after that child exits.
    owned = {result.get("pid"), result.get("launcher_pid")}
    return {pid: item for pid, item in contention.items() if pid not in owned}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="Preserved baseline binary directory")
    parser.add_argument("candidate", type=Path, help="Candidate binary directory")
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--modes", default="annotation,post-load,viewer")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 20:
        parser.error("Rounds must be between 1 and 20")
    modes = args.modes.split(",")
    if set(modes) - {"normals", "import", "annotation", "post-load", "viewer", "traced-viewer"}:
        parser.error("Unknown comparison mode")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    model = args.model.resolve(strict=True)
    binaries = {"baseline": args.baseline.resolve(strict=True), "candidate": args.candidate.resolve(strict=True)}
    report = {"method": "Warm input; rotating order; fresh process for each sample; no competing compiler or viewer",
              "model": str(model), "model_bytes": model.stat().st_size, "runs": [], "summary": {}}
    if sys.platform == "win32":
        report["power_plan"] = subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip()
    expected = {}
    ordinal = 0
    for mode in modes:
        for round_number in range(args.rounds):
            variants = ["baseline", "candidate"] if round_number % 2 == 0 else ["candidate", "baseline"]
            for variant in variants:
                accepted = False
                while not accepted:
                    while busy := competitors():
                        print("Waiting for idle machine:", busy[:4], flush=True)
                        time.sleep(5)
                    ordinal += 1
                    case = root / f"{ordinal:03d}-{mode}-{variant}-r{round_number + 1}"
                    case.mkdir()
                    viewer = mode in {"viewer", "traced-viewer"}
                    executable = binaries[variant] / ("woby.exe" if mode == "viewer" else
                        "woby_obj_e2e_viewer.exe" if viewer else
                        "woby_normals_benchmark.exe" if mode == "normals" else
                        "woby_obj_prototype_benchmark.exe" if mode == "import" else "woby_annotation_benchmark.exe")
                    if viewer:
                        command = [sys.executable, str(Path(__file__).with_name("e2e_run.py")), str(executable),
                                   str(model), "--output", str(case), "--settle-seconds", "2", "--render-seconds", "2"]
                    else:
                        with model.open("rb") as stream:
                            for _ in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                                pass
                        command = ([str(executable), "prototype", str(model), "16", "deferred"] if mode == "import" else
                                   [str(executable), str(model), "1"] if mode == "normals" else
                                   [str(executable), str(model), "1", mode])
                    print(f"Starting {case.name}", flush=True)
                    contention = {}
                    with (case / "stdout.log").open("w", encoding="utf-8") as out, (case / "stderr.log").open("w", encoding="utf-8") as err:
                        process = subprocess.Popen(command, stdout=out, stderr=err)
                        while process.poll() is None:
                            try:
                                owned = {process.pid, *(p.pid for p in psutil.Process(process.pid).children(recursive=True))}
                            except psutil.NoSuchProcess:
                                owned = {process.pid}
                            for item in competitors(owned):
                                contention[item["pid"]] = item
                            time.sleep(.25)
                    if process.returncode:
                        raise RuntimeError(f"Failed sample: {case}")
                    result = json.loads((case / ("result.json" if viewer else "stdout.log")).read_text())
                    if viewer:
                        if result.get("error"):
                            raise RuntimeError(result["error"])
                        contention = viewer_contention(contention, result)
                        elapsed = result["load_rpc_ms"]
                    else:
                        measured = sample_metrics(mode, result)
                        elapsed = measured["wall_ms"]
                        identity = mode if mode in {"normals", "import"} else "annotation"
                        if measured["output"] != expected.setdefault(identity, measured["output"]):
                            raise RuntimeError(f"Output differs: {case}")
                    accepted = not contention
                    record = {"mode": mode, "variant": variant, "round": round_number + 1,
                              "wall_ms": elapsed, "accepted": accepted, "contention": list(contention.values()),
                              "output": str(case), "command": command,
                              "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
                    if not viewer:
                        record.update({key: measured[key] for key in ("cpu_ms", "peak_commit_bytes") if key in measured})
                        record["output_identity"] = measured["output"]
                    report["runs"].append(record)
                    (root / "results.json").write_text(json.dumps(report, indent=2))
                    print(f"{case.name}: {elapsed / 1000:.3f} s; accepted={accepted}", flush=True)
    for mode in modes:
        report["summary"][mode] = {}
        for variant in binaries:
            rows = [r["wall_ms"] for r in report["runs"] if r["accepted"] and r["mode"] == mode and r["variant"] == variant]
            report["summary"][mode][variant] = {"median_ms": statistics.median(rows), "min_ms": min(rows), "max_ms": max(rows), "runs": len(rows)}
    (root / "results.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
