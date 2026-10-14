# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "nvidia-ml-py>=12", "Pillow>=11", "duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Run rotating fresh-process controls sequentially, without builds or profilers."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


def schedule(configurations, rounds):
    return [(round_number + 1, configuration)
            for round_number in range(rounds)
            for configuration in configurations[round_number % len(configurations):] + configurations[:round_number % len(configurations)]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--configurations", default="production-hidden,production-visible,traced-hidden,traced-visible,fast-hidden")
    args = parser.parse_args()
    root = args.output.resolve(); root.mkdir(parents=True, exist_ok=True)
    binaries = args.bin.resolve(strict=True)
    configurations = args.configurations.split(",")
    manifest = {"method": "Rotating order; sequential fresh processes; same input, warm cache, viewport and camera", "runs": []}
    for ordinal, (round_number, configuration) in enumerate(schedule(configurations, args.rounds), 1):
        prefix, visibility = configuration.split("-")
        executable = {"production": "woby.exe", "traced": "woby_obj_e2e_viewer.exe", "fast": "woby_obj_upload_viewer.exe",
                      "cleanfast": "woby_obj_fast_pacing_viewer.exe"}[prefix]
        output = root / f"{ordinal:03d}-r{round_number}-{configuration}"
        command = [sys.executable, str(Path(__file__).with_name("e2e_run.py")), str(binaries / executable),
                   str(args.model.resolve(strict=True)), "--output", str(output), "--settle-seconds", "3", "--render-seconds", "8"]
        if visibility == "hidden":
            command.append("--hidden")
        record = {"ordinal": ordinal, "round": round_number, "configuration": configuration, "output": str(output), "command": command}
        if (output / "result.json").exists():
            saved = json.loads((output / "result.json").read_text())
            if saved.get("error") or saved.get("exit_code") != 0:
                raise RuntimeError(f"Existing incomplete run: {output}")
            print(f"Reusing completed {configuration} round {round_number}", flush=True)
        else:
            print(f"Starting {configuration} round {round_number}", flush=True)
            output.mkdir(parents=True, exist_ok=True)
            with (output / "driver.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=900)
            if result.returncode:
                raise RuntimeError(f"Run failed: {output}; see driver.log")
            saved = json.loads((output / "result.json").read_text())
        record["load_rpc_ms"] = saved["load_rpc_ms"]
        record["executable_sha256"] = saved["executable_sha256"]
        manifest["runs"].append(record)
        (root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"Finished {configuration} round {round_number}: ready {saved['load_rpc_ms']/1000:.3f} s", flush=True)
        time.sleep(2)


if __name__ == "__main__":
    main()
