"""Serial fresh-process sweep; rotate variants and compare complete fingerprints."""
import argparse
import json
from pathlib import Path
from contract_test import run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--variants", nargs="+", choices=("baseline", "hybrid", "adaptive", "diagnostic", "split1", "split8"),
                        default=["baseline", "hybrid", "adaptive"])
    args = parser.parse_args()
    models = sorted(args.models.resolve().glob("*.obj"))
    assert models and args.rounds > 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    modes = args.variants
    references = {}
    with args.output.open("x", encoding="utf-8") as output:
        for iteration in range(args.rounds):
            for model in models:
                # Condition ordinary file-cache state; production RapidOBJ uses native unbuffered I/O.
                with model.open("rb") as source:
                    while source.read(8 * 1024 * 1024):
                        pass
                reference = references.get(model)
                shift = iteration % len(modes)
                for mode in modes[shift:] + modes[:shift]:
                    print(f"round={iteration+1} model={model.name} variant={mode}", flush=True)
                    row = run(args.bin.resolve(), mode, model, args.bin.resolve())
                    row["round"] = iteration + 1
                    if reference is not None:
                        for key in ("hashes", "bounds", "radius", "vertices", "indices", "nodes", "point_entries"):
                            assert reference[key] == row[key], (model, mode, key)
                    reference = row
                    references[model] = row
                    output.write(json.dumps(row) + "\n")
                    output.flush()
                    print(f"  load={row['load_ms']/1000:.3f}s map={row['stages']['map_loop_ms']/1000:.3f}s", flush=True)


if __name__ == "__main__":
    main()
