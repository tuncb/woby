# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Run isolated CPU, I/O, upload and counter-calibration controls in sequence."""
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
    args = parser.parse_args()
    root, binaries, model = args.output.resolve(), args.bin.resolve(strict=True), args.model.resolve(strict=True)
    root.mkdir(parents=True, exist_ok=True)
    scripts = Path(__file__).parent.resolve()
    commands = [
        ("parser", [sys.executable, str(scripts / "run.py"), str(binaries / "woby_obj_profile_avx2.exe"), str(model),
                    "--modes", "parser", "--workers", "1,2,4,8,16", "--rounds", "3", "--output", str(root / "parser.json")]),
        ("reads", [sys.executable, str(scripts / "run.py"), str(binaries / "woby_obj_profile_avx2.exe"), str(model),
                   "--modes", "read-cached,read-native", "--workers", "1,8,16", "--rounds", "3", "--output", str(root / "reads.json")]),
        ("copy", [sys.executable, str(scripts / "run.py"), str(binaries / "woby_obj_profile_avx2.exe"), str(model),
                  "--modes", "memory", "--workers", "1", "--rounds", "3", "--output", str(root / "copy.json")]),
        ("diagnostic", [str(binaries / "woby_obj_profile_diagnostic.exe"), "load", str(model), "16"]),
        ("instructions", [str(binaries / "woby_obj_profile_avx2.exe"), "load", str(model), "16", "sample"]),
        ("upload-capacity", [str(binaries / "woby_gpu_upload_benchmark.exe")]),
        ("fp-calibration", [sys.executable, str(scripts / "deep_tools.py"), str(model), "--executable", str(binaries / "woby_obj_profile_sse2.exe"),
                            "--output", str(root / "calibration"), "--mode", "flop-control", "--sets", "fp"]),
    ]
    records = []
    for name, command in commands:
        print("Starting " + name, flush=True)
        log = root / (name + ".log")
        started = time.monotonic()
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=2400)
        record = {"name": name, "command": command, "exit_code": result.returncode, "seconds": time.monotonic() - started}
        records.append(record)
        (root / "manifest.json").write_text(json.dumps(records, indent=2))
        print(json.dumps(record), flush=True)
        if result.returncode:
            raise RuntimeError(f"Control failed: {name}; see {log}")
        time.sleep(2)


if __name__ == "__main__":
    main()
