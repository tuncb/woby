"""Run isolated Release processes serially; append raw measurements as JSONL.

No cache eviction: preload the OBJ once per model/round before rotating backends.
RapidOBJ native deliberately bypasses this cache. Timers include opening/reading,
but exclude the harness's geometry validation and result destruction.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--backends", nargs="+", default=["rapid", "rapid-cached", "fast", "tiny", "tiny-opt", "tiny-typed", "tiny-typed-cache", "woby"])
    parser.add_argument("--threads", type=int, default=-1)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    models = sorted(args.models.resolve().glob("*.obj")) if args.models.is_dir() else [args.models.resolve()]
    if not models or args.rounds < 1:
        parser.error("provide at least one model and a positive round count")
    failures = 0
    with args.output.open("a", encoding="utf-8") as output:
        for round_index in range(args.rounds):
            for model in models:
                with model.open("rb") as source:
                    while source.read(8 * 1024 * 1024):
                        pass
                shift = round_index % len(args.backends)
                backends = args.backends[shift:] + args.backends[:shift]
                for backend in backends:
                    mode = "cached" if backend.endswith("-cached") else "native"
                    name = backend.removesuffix("-cached")
                    suffix = ".exe" if os.name == "nt" else ""
                    exe = args.bin.resolve() / f"woby_obj_benchmark_{mode}{suffix}"
                    print(f"round={round_index + 1} model={model.name} backend={backend}", flush=True)
                    begin = time.perf_counter()
                    process = subprocess.run([exe, name, model, str(args.threads)], capture_output=True, text=True, timeout=900)
                    try:
                        row = json.loads(process.stdout)
                    except json.JSONDecodeError:
                        row = {"ok": False, "file": str(model), "backend": name, "stdout": process.stdout}
                    row.update(round=round_index + 1, variant=backend, returncode=process.returncode,
                               process_seconds=time.perf_counter() - begin, stderr=process.stderr)
                    output.write(json.dumps(row) + "\n")
                    output.flush()
                    failures += not row["ok"] or process.returncode != 0
                    print(f"  ok={row['ok']} load_ms={row.get('load_ms')} peak_GiB={row.get('peak_working_set_bytes', 0) / 2**30:.3f}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
