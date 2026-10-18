# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6"]
# ///
"""Run one isolated workload and retain provenance, process samples, and profiler data."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import time

import psutil

UPROF = Path(r"C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe")
VTUNE = Path(r"C:\Program Files (x86)\Intel\oneAPI\vtune\2025.3\bin64\vtune.exe")
BASE = [(0x76, 0, 1_000_000), (0xC0, 0, 1_000_000)]
SETS = {
    "branch": BASE + [(0xC2, 0, 500_000), (0xC3, 0, 25_000), (0x1C7, 0, 25_000), (0xCA, 0, 25_000)],
    "data": BASE + [(0x29, 7, 1_000_000), (0x43, 1, 25_000), (0x43, 2, 25_000), (0x43, 8, 25_000)],
    "l2": BASE + [(0x64, 0xF0, 25_000), (0x64, 8, 25_000), (0x70, 0xFF, 25_000), (0x72, 0xFF, 25_000)],
    "tlb": BASE + [(0x45, 0x0F, 25_000), (0x45, 0xF0, 25_000), (0x47, 1, 25_000), (0x47, 2, 25_000)],
    "frontend": BASE + [(0x82, 0, 25_000), (0x83, 0, 25_000), (0xA9, 0, 500_000), (0x85, 7, 25_000)],
    "backend": BASE + [(0xAE, 2, 500_000), (0xAE, 4, 500_000), (0xAF, 0x0F, 500_000), (0xAE, 1, 500_000)],
    "prefetch": BASE + [(0x70,0xFF,25_000),(0x71,0xFF,25_000),(0x72,0xFF,25_000)],
    "math": BASE + [(0xCB,4,500_000),(0xD3,0,500_000),(0xD4,0,25_000)],
}


def write(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def process_times():
    result = {}
    for process in psutil.process_iter(["pid", "name", "cpu_times"]):
        try:
            cpu = process.info["cpu_times"]
            if cpu is not None and process.pid:
                result[process.pid] = (process.info["name"], cpu.user + cpu.system)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--memory-gib", type=float, default=48)
    parser.add_argument("--affinity", default="")
    parser.add_argument("--profile", choices=[*SETS,"timechart","vtune"])
    parser.add_argument("--sample-scale",type=int,default=1)
    args, workload = parser.parse_known_args()
    if not 1 <= args.sample_scale <= 1000:
        parser.error("sample scale must be between 1 and 1000")
    if workload and workload[0] == "--":
        workload.pop(0)
    exe = args.executable.resolve(strict=True); output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = args.name; destination = output / name
    destination.mkdir()  # Never replace an existing capture.
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    symbols = output / ("symbols-" + digest[:16])
    if not symbols.exists():
        symbols.mkdir()
        for suffix in (".exe", ".pdb"):
            path = exe.with_suffix(suffix)
            if path.exists():
                shutil.copy2(path, symbols / path.name)
        maps = list(exe.parents[2].glob(f"experiments/detector-profile/{exe.stem}.map"))
        for path in maps:
            shutil.copy2(path, symbols / path.name)
    if not (symbols / "dependencies.json").exists():
        dependencies = []
        for dll in exe.parent.glob("*.dll"):
            dll_hash = hashlib.sha256(dll.read_bytes()).hexdigest()
            shutil.copy2(dll, symbols / dll.name)
            entry = {"name":dll.name,"sha256":dll_hash}
            candidates = [exe.parents[2] / "vcpkg_installed/x64-windows/bin" / dll.with_suffix(".pdb").name]
            if dll.name == "manifold.dll" and os.environ.get("VCPKG_ROOT"):
                candidates.append(Path(os.environ["VCPKG_ROOT"]) / "buildtrees/manifold/x64-windows-rel/lib/manifold.pdb")
            for pdb in candidates:
                sibling = pdb.with_suffix(".dll")
                if pdb.exists() and sibling.exists() and hashlib.sha256(sibling.read_bytes()).hexdigest() == dll_hash:
                    shutil.copy2(pdb,symbols / pdb.name)
                    entry["pdb_sha256"] = hashlib.sha256(pdb.read_bytes()).hexdigest()
                    break
            dependencies.append(entry)
        write(symbols / "dependencies.json",dependencies)
    generated = exe.parents[2] / "experiments/detector-profile/generated"
    if generated.exists() and not (symbols / "generated").exists():
        shutil.copytree(generated,symbols / "generated")
    frozen = generated.parent / "harness"
    if frozen.exists() and not (symbols / "harness").exists():
        shutil.copytree(frozen,symbols / "harness")
    probe = destination / "probe.json"
    command = [str(exe), *workload, "--output", str(probe)]
    affinity = list(map(int, args.affinity.split(","))) if args.affinity else list(range(psutil.cpu_count()))
    if args.profile in SETS:
        prefix = [str(UPROF), "collect", "--affinity", ",".join(map(str, affinity))]
        for event, mask, interval in SETS[args.profile]:
            prefix += ["-e", f"event=pmcx{event:03x},umask=0x{mask:x},interval={interval*args.sample_scale},user=1,os=0"]
        command = prefix + ["-o", str(destination / "session"), *command]
    elif args.profile == "timechart":
        command = [str(UPROF),"timechart","-e","frequency,power","-t","100","-o",str(destination / "timechart"),*command]
    elif args.profile == "vtune":
        command = [str(VTUNE),"-collect","hotspots","-knob","sampling-mode=sw","-knob","enable-stack-collection=true",
                   "-result-dir",str(destination / "vtune"),"--",*command]
    metadata = {"utc": datetime.now(timezone.utc).isoformat(), "command": command,
                "executable_sha256": digest, "symbols": str(symbols), "affinity": affinity,
                "profile": args.profile, "timeout_seconds": args.timeout, "memory_limit_gib": args.memory_gib,
                "sample_scale":args.sample_scale,
                "python_version":platform.python_version(),"psutil_version":psutil.__version__,
                "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "workspace_harness_hashes_at_launch": {p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(__file__).parent.iterdir()
                    if p.suffix in (".py",".cpp",".h") or p.name == "CMakeLists.txt"},
                "memory_scope": "Sampled process RSS/private bytes; OS lifetime peaks include loading. Stage boundary counters are in probe.json.",
                "power_plan": subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip()}
    battery = psutil.sensors_battery()
    metadata["battery"] = {"percent":battery.percent,"power_plugged":battery.power_plugged} if battery else None
    write(destination / "metadata.json", metadata)
    parent = psutil.Process(); prior_affinity = parent.cpu_affinity()
    started = time.perf_counter(); observer_utc = datetime.now(timezone.utc)
    observations = []; external = []; previous = process_times(); last_external = started
    metadata["observer_start_qpc_ns"] = round(started * 1e9)
    metadata["observer_start_utc"] = observer_utc.isoformat()
    metadata["timezone_offset_seconds"] = datetime.now().astimezone().utcoffset().total_seconds()
    stop_reason = None
    print(f"START {name}", flush=True)
    with (destination / "console.log").open("w", encoding="utf-8") as log:
        try:
            parent.cpu_affinity(affinity)
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        finally:
            parent.cpu_affinity(prior_affinity)
        controller = psutil.Process(child.pid)
        while child.poll() is None:
            now = time.perf_counter(); elapsed = now - started
            try:
                processes = [controller, *controller.children(recursive=True)]
            except psutil.NoSuchProcess:
                break
            own_ids = {parent.pid, *[p.pid for p in processes]}
            for process in processes:
                try:
                    if process.name().lower() != exe.name.lower():
                        continue
                    memory = process.memory_info(); cpu = process.cpu_times()
                    observations.append({"elapsed_s": elapsed, "pid": process.pid, "rss": memory.rss,
                        "private": getattr(memory, "private", None), "peak_rss": getattr(memory, "peak_wset", None),
                        "page_faults": getattr(memory, "num_page_faults", None), "cpu_s": cpu.user+cpu.system,
                        "threads": process.num_threads(), "affinity": process.cpu_affinity()})
                    if memory.rss > args.memory_gib * 1024**3 or psutil.virtual_memory().available < 4 * 1024**3:
                        stop_reason = "memory_guard"
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    pass
            if now-last_external >= 1:
                current = process_times(); busy = []
                for pid, (process_name, cpu) in current.items():
                    if pid not in own_ids and pid in previous:
                        delta = cpu - previous[pid][1]
                        if delta > .02:
                            busy.append({"pid": pid, "name": process_name, "cpu_s": delta})
                external.append({"elapsed_s": elapsed, "interval_s": now-last_external,
                                 "busy": sorted(busy, key=lambda p: p["cpu_s"], reverse=True)[:12]})
                previous = current; last_external = now
            if elapsed > args.timeout:
                stop_reason = "timeout"
            if stop_reason:
                for process in reversed(processes):
                    try:
                        process.kill()
                    except psutil.NoSuchProcess:
                        pass
                break
            time.sleep(.25)
        child.wait()
    metadata.update(exit_code=child.returncode, stop_reason=stop_reason, wall_s=time.perf_counter()-started,
                    executable_unchanged=hashlib.sha256(exe.read_bytes()).hexdigest() == digest)
    write(destination / "resources.json", {"samples": observations, "external_cpu": external})
    write(destination / "metadata.json", metadata)
    if child.returncode == 0 and probe.exists() and args.profile in SETS:
        with (destination / "translation.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(UPROF), "report", "-i", str(destination / "session"), "--cutoff", "0",
                                     "--symbol-path", str(symbols)], stdout=log, stderr=subprocess.STDOUT,
                                    input="\n", text=True, timeout=1200)
        metadata["translation_exit_code"] = result.returncode
        metadata["has_sample_database"] = (destination / "session/cpu.db").exists()
        write(destination / "metadata.json", metadata)
    if probe.exists():
        result = json.loads(probe.read_text())
        print(json.dumps({"name": name, "mesh": result.get("mesh"), "samples": [{"op": s["operation"], "status": s["status"], "error": s.get("error")} for s in result["samples"]],
                          "times_ms": [{"op": e["name"], "wall": round(e["wall_ms"],2), "cpu": round(e["cpu_ms"],2)} for e in result["events"] if e["name"].startswith("op.")],
                          "stop_reason": stop_reason}), flush=True)
    else:
        print(json.dumps({"name": name, "exit_code": child.returncode, "stop_reason": stop_reason, "probe_missing": True}), flush=True)
    return 0 if child.returncode == 0 and probe.exists() and not stop_reason else 2


if __name__ == "__main__":
    raise SystemExit(main())
