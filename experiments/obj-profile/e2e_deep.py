# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Collect diagnostic profiles sequentially. Preserve successful and failed attempts."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", default="branch,cache,fp,production-branch,vtune,nsys,nsys-gpu,nsys-wddm,timechart,memory,features")
    args = parser.parse_args()
    root = args.output.resolve(); root.mkdir(parents=True, exist_ok=True)
    executable = args.bin.resolve(strict=True) / "woby_obj_e2e_viewer.exe"
    records = json.loads((root / "manifest.json").read_text()) if (root / "manifest.json").exists() else []
    for case in args.cases.split(","):
        output = root / case
        measured_exe = args.bin.resolve(strict=True) / "woby.exe" if case.startswith("production-") else executable
        command = [sys.executable, str(Path(__file__).with_name("e2e_run.py")), str(measured_exe),
                   str(args.model.resolve(strict=True)), "--output", str(output), "--settle-seconds", "1", "--render-seconds", "3"]
        if case not in ("features", "production-features"):
            command.append("--hidden")
        if case in ("branch", "cache", "fp", "production-branch"):
            command += ["--profiler", "uprof", "--event-set", "branch" if case == "production-branch" else case]
        elif case == "memory":
            command += ["--memory-map", "--reload-cycles", "3", "--settle-seconds", "5"]
        elif case in ("features", "production-features"):
            command += ["--render-features", "--navigation"]
        else:
            command += ["--profiler", case.removesuffix("-retry")]
            if case == "ngfx":
                command += ["--settle-seconds", "30", "--render-seconds", "10"]
        print("Collecting " + case, flush=True)
        started = time.monotonic()
        if (output / "result.json").exists():
            record = json.loads((output / "result.json").read_text())
            status = 1 if "error" in record else 0
        else:
            output.mkdir(parents=True, exist_ok=True)
            with (output / "driver.log").open("w", encoding="utf-8") as log:
                try:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=1200)
                    status = result.returncode
                except subprocess.TimeoutExpired:
                    status = -1
            record = json.loads((output / "result.json").read_text()) if (output / "result.json").exists() else {}
        entry = {"case": case, "command": command, "exit_code": status, "seconds": time.monotonic() - started,
                 "error": record.get("error"), "output": str(output)}
        records = [r for r in records if r['case'] != case] + [entry]
        (root / "manifest.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        print(json.dumps({k: entry[k] for k in ("case", "exit_code", "seconds", "error")}), flush=True)
        time.sleep(2)


if __name__ == "__main__":
    main()
