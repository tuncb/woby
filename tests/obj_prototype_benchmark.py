"""Run fresh-process, rotating-order CPU comparisons with ordered geometry checks.

Run on an idle machine with a Release benchmark executable. This measures CPU
loading, not GPU upload or first visible frame. Generated fixtures share one
unique temporary root, including the small-file directory, and are always removed.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile


def fixtures(root: Path):
    small = root / "small-files"
    small.mkdir()
    triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
    for index in range(200):
        (small / f"{index:04}.obj").write_text(triangle, encoding="utf-8")
    mixed = root / "mixed-weighted.obj"
    with mixed.open("w", encoding="utf-8", newline="\n") as output:
        for index in range(50000):
            # Weighted syntax routes the reference reader through its fallback.
            # Distinct indices and a large coordinate origin exercise ownership.
            x = 1000000000 + index * 2
            output.write(f"v {x} 0 0 1\nv {x+1} 0 0 1\nv {x} 1 0 1\n")
        output.write("g faces\n")
        for index in range(50000):
            first = index * 3 + 1
            output.write(f"f {first} {first+1} {first+2}\n")
        output.write("g curve\ncstype rat bezier\ndeg 2\ncurv 0 1 -3 \\\n -2 -1\nparm u 0 1\nend\n")
    return [small, mixed]


def warm(path: Path):
    for file in sorted(path.glob("*.obj")) if path.is_dir() else [path]:
        with file.open("rb") as source:
            while source.read(4 * 1024 * 1024):
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("models", type=Path, nargs="*")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument("--reference-executable", type=Path,
                        help="Also compare the prototype from a previously built executable")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    executable = args.executable.resolve(strict=True)
    configurations = [(executable, "legacy", "legacy"), (executable, "prototype", "prototype")]
    if args.reference_executable:
        configurations.insert(1, (args.reference_executable.resolve(strict=True), "prototype", "prototype_before"))
    models = [model.resolve(strict=True) for model in args.models]
    records = []
    with tempfile.TemporaryDirectory(prefix="woby_rapidobj_comparison_") as temporary:
        corpus = fixtures(Path(temporary).resolve()) + models
        for round_index in range(args.rounds):
            rotation = round_index % len(configurations)
            modes = configurations[rotation:] + configurations[:rotation]
            for model in corpus:
                for program, mode, label in modes:
                    warm(model)
                    run = subprocess.run([str(program), mode, str(model), str(args.workers)],
                                         check=True, text=True, capture_output=True, timeout=600)
                    record = json.loads(run.stdout)
                    record.update(model=model.name, round=round_index + 1, mode=label)
                    records.append(record)
                    print(json.dumps(record), flush=True)
                    # Persist completed measurements even if a later run fails.
                    args.output.parent.mkdir(parents=True, exist_ok=True)
                    args.output.write_text(json.dumps({"records": records}, indent=2), encoding="utf-8")
        summaries = []
        for model in corpus:
            runs = [record for record in records if record["model"] == model.name]
            assert len({record["fingerprint"] for record in runs}) == 1, f"Geometry mismatch: {model.name}"
            summary = {"model": model.name, "fingerprint": runs[0]["fingerprint"]}
            for _, _, mode in configurations:
                selected = [record for record in runs if record["mode"] == mode]
                summary[mode] = {key: statistics.median(record[key] for record in selected)
                                 for key in ("cpu_load_ms", "peak_working_set_bytes", "peak_commit_bytes",
                                             "retained_mesh_buffer_capacity_bytes") if key in selected[0]}
            summaries.append(summary)
        args.output.write_text(json.dumps({"method": "Release CPU load; fresh processes; input warmed before each process; rotating order",
                                           "records": records, "summaries": summaries}, indent=2), encoding="utf-8")
        print(json.dumps({"summaries": summaries}, indent=2), flush=True)


if __name__ == "__main__":
    main()
