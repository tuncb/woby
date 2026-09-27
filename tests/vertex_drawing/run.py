"""Serial alternating fresh-process A/B sweep; no build/test work during timing."""
import argparse
import json
from pathlib import Path
import subprocess
from contract_test import compare_images, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--filter", default="*.obj")
    parser.add_argument("--modes", nargs="+", choices=("current", "shader", "flat"), default=["current", "shader", "flat"])
    args = parser.parse_args()
    models = sorted(args.models.resolve().glob(args.filter), key=lambda path: path.stat().st_size)
    assert models and args.rounds > 0
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with (output / "runs.jsonl").open("x") as stream:
        for iteration in range(args.rounds):
            for model in models:
                # Load cost is reported but excluded from the marker comparison.
                with model.open("rb") as source:
                    while source.read(8 * 1024 * 1024):
                        pass
                rows = {}
                model_output = output / f"round-{iteration+1}" / model.stem
                shift = iteration % len(args.modes)
                for mode in args.modes[shift:] + args.modes[:shift]:
                    print(f"round={iteration+1} model={model.name} mode={mode}", flush=True)
                    try:
                        row = run(args.binary.resolve(), model, mode, model_output / mode,
                                  quick=False, seconds=args.seconds)
                    except (RuntimeError, subprocess.TimeoutExpired) as error:
                        row = {"model": str(model), "mode": mode, "ok": False, "error": str(error)}
                    row["round"] = iteration + 1
                    stream.write(json.dumps(row) + "\n")
                    stream.flush()
                    rows[mode] = row
                    if row["ok"]:
                        print(f"  enable_cpu={row['enable_cpu_ms']:.2f}ms ready={row['enable_ready_ms']:.2f}ms "
                              f"gpu_points={row['scenarios'][0]['gpu_markers']['median_ms']:.3f}ms", flush=True)
                    else:
                        print(f"  FAILED: {row['error']}", flush=True)
                if "current" in rows and rows["current"]["ok"]:
                    comparisons = {}
                    for mode, row in rows.items():
                        if mode == "current" or not row["ok"]:
                            continue
                        for key in ("point_entries", "point_hash", "vertices", "indices", "groups"):
                            assert rows["current"][key] == row[key], (model, key)
                        comparisons[mode] = {scenario["name"]: compare_images(model_output / "current", model_output / mode, scenario["name"])
                                             for scenario in rows["current"]["scenarios"]}
                    (model_output / "image-comparison.json").write_text(json.dumps(comparisons, indent=2))
                    print(f"  image differences: {comparisons}", flush=True)


if __name__ == "__main__":
    main()
