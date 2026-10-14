"""Compare complete imports in fresh processes, with alternating path order."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    out = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            out.update(block)
    return out.hexdigest()


def warm(model):
    with model.open("rb") as stream:
        for _ in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            pass


def cpu(args):
    exe = args.bin / "woby_position_mapping.exe"
    records = []
    for preparation, rounds in (("deferred", args.rounds), ("eager", 3)):
        for rd in range(1, rounds + 1):
            for label in (("fallback", "staged") if rd % 2 else ("staged", "fallback")):
                warm(args.model)
                mode = "prototype" if label == "fallback" else "forced"
                command = [str(exe), mode, str(args.model), "0"] + (["deferred"] if preparation == "deferred" else [])
                process = subprocess.run(command, capture_output=True, text=True, check=True, timeout=600)
                record = json.loads(process.stdout)
                record.update(configuration=label, round=rd, preparation=preparation, command=command)
                if record["corner_mapping"]["staged"] != (label == "staged"):
                    raise ValueError("The requested mapping path was not selected")
                records.append(record)
                (args.output / "cpu.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
                print(json.dumps({key: record[key] for key in ("configuration", "round", "preparation", "cpu_load_ms", "peak_working_set_bytes", "peak_commit_bytes", "fingerprint")}), flush=True)
    for preparation in ("deferred", "eager"):
        selected = [r for r in records if r["preparation"] == preparation]
        if len({r["fingerprint"] for r in selected}) != 1:
            raise ValueError("The full mesh fingerprints differ")


def viewer(args):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "obj-profile"))
    from e2e_analyze import summarize_run
    programs = {"fallback": "woby.exe" if args.kind == "native" else "woby_obj_e2e_viewer.exe",
                "staged": "woby_position_mapping_" + args.kind + ".exe"}
    if args.matched:
        programs["fallback"] = "woby_position_mapping_" + args.kind + "_reference.exe"
    records = []
    driver = Path(__file__).resolve().parents[1] / "obj-profile/e2e_run.py"
    for rd in range(1, args.rounds + 1):
        for label in (("fallback", "staged") if rd % 2 else ("staged", "fallback")):
            output = args.output / f"r{rd}-{label}"
            output.mkdir()
            command = ["uv", "run", str(driver), str(args.bin / programs[label]), str(args.model),
                       "--output", str(output), "--hidden", "--settle-seconds", "1", "--render-seconds", "2"]
            print(f"Starting {args.kind} round {rd} {label}", flush=True)
            with (output / "driver.log").open("w", encoding="utf-8") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
            result = summarize_run(output / "result.json")
            result.update(configuration=label, round=rd, command=command)
            records.append(result)
            (args.output / "summary.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
            if result.get("error"):
                raise ValueError(result["error"])
            print(json.dumps({key: result[key] for key in ("configuration", "round", "cpu_import_ms", "load_rpc_ms", "milestones_ms", "process") if key in result}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("cpu", "native", "traced"))
    parser.add_argument("bin", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--matched", action="store_true", help="Use matching viewer build controls")
    args = parser.parse_args()
    if not 1 <= args.rounds <= 20:
        parser.error("--rounds must be one to 20")
    args.bin = args.bin.resolve(strict=True)
    args.model = args.model.resolve(strict=True)
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {"input": str(args.model), "input_bytes": args.model.stat().st_size,
                "input_sha256": digest(args.model), "kind": args.kind, "rounds": args.rounds,
                "method": "Release; fresh processes; warm file; alternating order; default parser and gather worker counts",
                "matched_viewer_builds": args.matched,
                "executables": {path.name: digest(path) for path in args.bin.glob("*.exe")
                                if path.name in ("woby.exe", "woby_obj_e2e_viewer.exe", "woby_position_mapping.exe",
                                                 "woby_position_mapping_native.exe", "woby_position_mapping_traced.exe",
                                                 "woby_position_mapping_native_reference.exe", "woby_position_mapping_traced_reference.exe")}}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    cpu(args) if args.kind == "cpu" else viewer(args)
