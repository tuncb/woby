"""Serial fresh-process measurements; screenshots are taken after timing ends."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def run(binary, model, output):
    output = output.resolve()
    env = dict(os.environ, WOBY_FIRST_DISPLAY_OUTPUT=str(output))
    command = [str(binary.resolve()), "--file", str(model.resolve()), "--instance", "first-display-probe",
               "--log-level", "info", "--log-file", str(output.with_suffix(".log"))]
    begin = time.perf_counter()
    process = subprocess.run(command, cwd=output.parent, env=env, capture_output=True,
                             text=True, timeout=600)
    assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
    row = json.loads(output.read_text())
    row["process_wall_ms"] = (time.perf_counter() - begin) * 1000
    row["file"] = str(model.resolve())
    assert row["load_to_first_complete_ms"] >= row["model_file_ms"] > 0
    assert row["vertices"] > 0 and row["triangles"] > 0
    assert all(value >= 0 for value in row["stages"].values())
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    models = sorted(args.models.resolve().glob("*.obj"))
    assert models and args.rounds > 0
    with (args.output / "runs.jsonl").open("x") as stream:
        for iteration in range(args.rounds):
            for model in models:
                print(f"round={iteration+1} model={model.name}", flush=True)
                row = run(args.bin, model, args.output / f"{model.stem}-{iteration+1}.json")
                row["round"] = iteration + 1
                stream.write(json.dumps(row) + "\n")
                stream.flush()
                print(f"  first_complete={row['load_to_first_complete_ms']/1000:.3f}s "
                      f"cpu_load={row['cpu_load_ms']/1000:.3f}s", flush=True)


if __name__ == "__main__":
    main()
