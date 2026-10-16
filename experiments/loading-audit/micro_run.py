# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6"]
# ///
"""Run parser-only affinity controls and small, unmultiplexed PMC sets."""
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

import psutil

UPROF = Path(r"C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe")
EVENT_XML = Path(r"C:\Program Files\AMD\AMDuProf\bin\Data\Events\0x19_0x5.xml")
BASE = [(0x76, 0, 2_000_000), (0xC0, 0, 2_000_000)]
SETS = {
    "branch": BASE + [(0xC2, 0, 500_000), (0xC3, 0, 25_000), (0x1C7, 0, 25_000), (0xCA, 0, 25_000)],
    "data": BASE + [(0x29, 7, 1_000_000), (0x43, 1, 25_000), (0x43, 2, 25_000), (0x43, 8, 25_000)],
    "l2": BASE + [(0x64, 0xF0, 25_000), (0x64, 8, 25_000), (0x70, 0xFF, 25_000), (0x72, 0xFF, 25_000)],
    "tlb": BASE + [(0x45, 0x0F, 25_000), (0x45, 0xF0, 25_000), (0x47, 1, 25_000), (0x47, 2, 25_000)],
    "frontend": BASE + [(0x82, 0, 25_000), (0x83, 0, 25_000), (0xA9, 0, 500_000), (0x85, 7, 25_000)],
    "backend": BASE + [(0xAE, 2, 500_000), (0xAE, 4, 500_000), (0xAF, 0x0F, 500_000), (0xAE, 1, 500_000)],
}


def write(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def decode_topology(data):
    cores, caches, position = [], [], 0
    while position < len(data):
        relationship = int.from_bytes(data[position:position + 4], "little")
        size = int.from_bytes(data[position + 4:position + 8], "little")
        if size < 8 or position + size > len(data):
            raise ValueError("Invalid Windows processor topology record")
        body = data[position + 8:position + size]
        if relationship == 0:
            count = int.from_bytes(body[22:24], "little")
            groups = []
            for i in range(count):
                offset = 24 + i * 16
                mask = int.from_bytes(body[offset:offset + 8], "little")
                group = int.from_bytes(body[offset + 8:offset + 10], "little")
                groups.append({"group": group, "mask": mask, "logical_ids": [j for j in range(64) if mask & (1 << j)]})
            cores.append({"flags": body[0], "groups": groups})
        elif relationship == 2:
            mask = int.from_bytes(body[32:40], "little")
            caches.append({"level": body[0], "associativity": body[1], "line_bytes": int.from_bytes(body[2:4], "little"),
                "bytes": int.from_bytes(body[4:8], "little"), "type": int.from_bytes(body[8:12], "little"),
                "mask": mask, "logical_ids": [j for j in range(64) if mask & (1 << j)]})
        position += size
    return {"cores": cores, "caches": caches}


def topology():
    function = ctypes.WinDLL("kernel32", use_last_error=True).GetLogicalProcessorInformationEx
    function.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
    function.restype = wintypes.BOOL
    size = wintypes.DWORD()
    function(0xFFFF, None, ctypes.byref(size))
    buffer = ctypes.create_string_buffer(size.value)
    if not function(0xFFFF, buffer, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    return dict(decode_topology(buffer.raw), source="Windows GetLogicalProcessorInformationEx (x64)")


def affinity_sets(info):
    if any(group["group"] != 0 for core in info["cores"] for group in core["groups"]):
        raise ValueError("This experiment requires a single Windows processor group")
    logical = [core["groups"][0]["logical_ids"] for core in info["cores"]]
    return {"all": sorted(n for ids in logical for n in ids), "physical": [ids[0] for ids in logical],
            "four-core-smt": sorted(n for ids in logical[:4] for n in ids)}


def warm(model):
    with model.open("rb") as source:
        while source.read(8 * 1024**2):
            pass


def run_inherited(command, affinity, output, timeout=900):
    parent = psutil.Process()
    original = parent.cpu_affinity()
    start = time.perf_counter()
    try:
        parent.cpu_affinity(affinity)
        with output.open("w", encoding="utf-8") as log:
            child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            actual = psutil.Process(child.pid).cpu_affinity()
            try:
                code = child.wait(timeout=timeout)
            except BaseException:
                child.kill(); child.wait(); raise
        if code:
            raise RuntimeError(f"Command failed ({code}); see {output}")
    finally:
        parent.cpu_affinity(original)
    if sorted(actual) != sorted(affinity):
        raise RuntimeError("Child did not inherit the requested affinity")
    return {"process_wall_ms": (time.perf_counter() - start) * 1000, "inherited_affinity": actual}


def record_from_log(path):
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if line.startswith("{"):
            value = json.loads(line)
            if "events" in value and "compiler_isa" in value:
                return value
    raise ValueError("No parser output in " + str(path))


def timing(exe, model, output, masks):
    configurations = [("w8-physical", 8, masks["physical"]), ("w8-all", 8, masks["all"]),
                      ("w8-four-core-smt", 8, masks["four-core-smt"]), ("w16-all", 16, masks["all"])]
    path = output / "affinity-timings.json"
    data = {"method": "Fresh processes; inherited affinity fixed before launch; warm input; three rounds in rotating order; no profiler", "records": []}
    for round_index in range(3):
        order = configurations[round_index:] + configurations[:round_index]
        for name, workers, mask in order:
            warm(model)
            console = output / f"timing-{round_index + 1}-{name}.log"
            extra = run_inherited([str(exe), "parser", str(model), str(workers)], mask, console)
            record = record_from_log(console)
            record.update(extra, configuration=name, round=round_index + 1, affinity=mask)
            data["records"].append(record)
            write(path, data)
            phase = next(row for row in record["events"] if row["name"] == "parser_total")
            print(json.dumps({"configuration": name, "round": round_index + 1, "wall_ms": phase["wall_ms"], "cpu_ms": phase["cpu_ms"]}), flush=True)
    fingerprints = {r["geometry"]["fingerprint"] for r in data["records"]}
    if len(fingerprints) != 1:
        raise ValueError("Affinity control fingerprints differ")


def collect(exe, model, output, masks):
    tasks = [("branch", 16, "all", i) for i in range(1, 4)]
    tasks += [("data", 16, "all", i) for i in range(1, 3)]
    tasks += [(kind, 16, "all", 1) for kind in ("l2", "tlb", "frontend", "backend")]
    tasks += [(kind, 8, "physical", 1) for kind in ("branch", "data", "frontend")]
    tasks += [(kind, 8, "four-core-smt", 1) for kind in ("branch", "data", "frontend")]
    tasks += [("backend", 8, mask_name, 1) for mask_name in ("physical", "four-core-smt")]
    manifest_path = output / "counter-manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"captures": []}
    completed = {row["name"] for row in manifest["captures"]}
    for kind, workers, mask_name, round_index in tasks:
        name = f"{kind}-w{workers}-{mask_name}-r{round_index}"
        if name in completed:
            continue
        session = output / name
        if session.exists():
            raise ValueError("Incomplete capture directory already exists: " + str(session))
        command = [str(UPROF), "collect", "--affinity", ",".join(str(cpu) for cpu in masks[mask_name])]
        for event, mask, interval in SETS[kind]:
            command += ["-e", f"event=pmcx{event:03x},umask=0x{mask:x},interval={interval},user=1,os=0"]
        command += ["-o", str(session), str(exe), "parser", str(model), str(workers)]
        warm(model)
        print("Capturing " + name, flush=True)
        extra = run_inherited(command, masks[mask_name], output / (name + "-console.log"))
        record = record_from_log(output / (name + "-console.log"))
        if not (session / "session.uprof").exists():
            raise ValueError("Profiler did not create a session: " + name)
        symbols = session / "matching-symbols"; symbols.mkdir()
        for suffix in (".exe", ".pdb"):
            shutil.copy2(exe.with_suffix(suffix), symbols / exe.with_suffix(suffix).name)
        write(output / (name + "-probe.json"), record)
        print("Translating " + name, flush=True)
        with (output / (name + "-translation.log")).open("w", encoding="utf-8") as log:
            subprocess.run([str(UPROF), "report", "-i", str(session), "--cutoff", "0", "--symbol-path", str(symbols)],
                           stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
        if not (session / "cpu.db").exists():
            raise ValueError("No translated native sample database: " + name)
        manifest["captures"].append(dict(extra, name=name, kind=kind, workers=workers, placement=mask_name, affinity=masks[mask_name],
            round=round_index, session=str(session), probe=str(output / (name + "-probe.json"))))
        write(manifest_path, manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path); parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path); parser.add_argument("--steps", default="topology,timing,collect")
    args = parser.parse_args()
    exe, model, output = args.executable.resolve(strict=True), args.model.resolve(strict=True), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    steps = args.steps.split(",")
    if "topology" in steps:
        write(output / "topology.json", topology())
    info = json.loads((output / "topology.json").read_text()); masks = affinity_sets(info)
    provenance = output / "provenance.json"
    if not provenance.exists():
        with model.open("rb") as source:
            model_digest = hashlib.file_digest(source, "sha256").hexdigest()
        write(provenance, {"captured_utc": datetime.now(timezone.utc).isoformat(), "executable": str(exe),
            "executable_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "model": str(model),
            "model_sha256": model_digest, "model_bytes": model.stat().st_size, "affinities": masks,
            "event_sets": SETS, "event_xml_sha256": hashlib.sha256(EVENT_XML.read_bytes()).hexdigest(),
            "power_plan": subprocess.check_output(["powercfg", "/getactivescheme"], text=True).strip()})
        shutil.copy2(EVENT_XML, output / "amd-family19-model50-events.xml")
    elif json.loads(provenance.read_text())["executable_sha256"] != hashlib.sha256(exe.read_bytes()).hexdigest():
        raise ValueError("Executable differs from campaign provenance")
    metadata = json.loads(provenance.read_text())
    metadata["event_sets"] = SETS
    metadata["minimum_pmc_interval"] = 25_000
    write(provenance, metadata)
    if "timing" in steps:
        timing(exe, model, output, masks)
    if "collect" in steps:
        collect(exe, model, output, masks)


if __name__ == "__main__":
    main()
