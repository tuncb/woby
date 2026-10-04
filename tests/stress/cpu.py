"""Run the existing opt-in CPU detector benchmark serially with memory/deadline guards."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import threading
import time

from run import GIB, add_memory_limit_arguments, monitor, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("models", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--select", action="append", required=True)
    parser.add_argument("--workload", choices=("detectors", "detectors-expanded", "intersections"), default="detectors")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=300)
    add_memory_limit_arguments(parser)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 100 or args.timeout < 1:
        parser.error("Repetitions must be 1..100 and the deadline must be positive")
    executable, models, output = (p.resolve() for p in (args.executable, args.models, args.output))
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / "manifest.json", dict(
        arguments={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
        harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), python=sys.version,
        started_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        max_private_gib=args.max_private_gib, min_available_gib=args.min_available_gib))
    for model in sorted(models.rglob("*.obj"), key=lambda p: p.stat().st_size):
        if not any(value in model.name for value in args.select):
            continue
        case = output / (args.workload+"-"+model.stem)
        case.mkdir()
        started = time.perf_counter()
        with (case / "stdout.txt").open("w", encoding="utf-8") as out, (case / "stderr.txt").open("w", encoding="utf-8") as err:
            command = [str(executable), args.workload, str(model), str(args.repetitions)]
            process = subprocess.Popen(command, cwd=case, stdout=out, stderr=err)
            state = dict(process=process, started=started, phase=args.workload, stop=threading.Event(),
                         cap=args.max_private_gib*GIB, floor=args.min_available_gib*GIB, resources=[])
            watcher = threading.Thread(target=monitor, args=(state,), daemon=True)
            watcher.start()
            try:
                process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                state["termination"] = "deadline"
                process.kill()
                process.wait(timeout=20)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=20)
                state["stop"].set()
                watcher.join(timeout=3)
            summary = dict(command=command, model_bytes=model.stat().st_size, seconds=time.perf_counter()-started,
                           exit_code=process.returncode, status=state.get("termination", "completed" if process.returncode == 0 else "rejected" if process.returncode == 1 else "process_exit"))
            if state["resources"]:
                summary["peak_private_bytes"] = max(s["private"] for s in state["resources"])
                summary["peak_rss_bytes"] = max(s["peak_rss"] for s in state["resources"])
            write_json(case / "case.json", summary)
            write_json(case / "resources.json", state["resources"])
            print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
