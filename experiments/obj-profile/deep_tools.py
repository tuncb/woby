# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Collect sequential hardware profiles and summarize their native samples."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import duckdb
import pefile
import capstone

UPROF = Path(r"C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe")
BASE = [(0x76, 0, 1_000_000), (0xC0, 0, 1_000_000)]
SETS = {
    "branch": BASE + [(0xC2, 0, 500_000), (0xC3, 0, 25_000), (0x43, 8, 25_000), (0x45, 0xF0, 25_000)],
    "cache": BASE + [(0x29, 7, 1_000_000), (0x43, 1, 25_000), (0x43, 2, 25_000), (0x43, 8, 25_000)],
    "fp": BASE + [(0x03, 0xF, 250_000)],
}


def record_from_log(path):
    content = path.read_text(encoding="utf-8-sig", errors="replace")
    for line in content.splitlines():
        if line.startswith('{'):
            value = json.loads(line)
            if "compiler_isa" in value and "events" in value:
                return value
    raise ValueError(f"No probe record: {path}")


def preserve_capture_symbols(exe, session, probe_path, target, fresh):
    symbols = session / "matching-symbols"
    if fresh:
        symbols.mkdir(exist_ok=True)
        for suffix in (".exe", ".pdb"):
            shutil.copy2(exe.with_suffix(suffix), symbols / exe.with_suffix(suffix).name)
        target["executable_sha256"] = hashlib.sha256((symbols / exe.name).read_bytes()).hexdigest()
        probe_path.write_text(json.dumps(target, indent=2), encoding="utf-8")
    else:
        snapshot = symbols / exe.name
        if not snapshot.exists() or not probe_path.exists():
            raise ValueError("Existing capture lacks preserved executable provenance; use a new output directory")
        saved = json.loads(probe_path.read_text(encoding="utf-8"))
        actual = hashlib.sha256(snapshot.read_bytes()).hexdigest()
        if saved.get("executable_sha256") != actual:
            raise ValueError("Preserved executable does not match the capture hash")
        if hashlib.sha256(exe.read_bytes()).hexdigest() != actual:
            print("Existing capture uses its preserved executable; the current build differs", flush=True)
    return symbols


def collect(exe, output, kind, mode, model):
    name = f"uprof-{mode}-{exe.stem.rsplit('_',1)[-1]}-{kind}"
    session = output / name
    command = [str(UPROF), "collect"]
    for event, mask, interval in SETS[kind]:
        command += ["-e", f"event=pmcx{event:03x},umask=0x{mask:x},interval={interval},user=1,os=0"]
    command += ["-o", str(session), str(exe), mode, str(model), "16"]
    log = output / (name + "-console.log")
    fresh = not session.exists()
    before_hash = hashlib.sha256(exe.read_bytes()).hexdigest() if fresh else None
    if fresh:
        print("Collecting " + name, flush=True)
        with log.open("w", encoding="utf-8") as stream:
            subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=600)
    else:
        print("Reusing collected data " + name, flush=True)
    # uProf can exit with code zero after an argument error. Require both data and a target record.
    target = record_from_log(log)
    if fresh and hashlib.sha256(exe.read_bytes()).hexdigest() != before_hash:
        raise ValueError("Executable changed during capture; its symbols cannot be trusted")
    symbols = preserve_capture_symbols(exe, session, output / (name + "-probe.json"), target, fresh)
    with (output / (name + "-translation.log")).open("w", encoding="utf-8") as stream:
        subprocess.run([str(UPROF), "report", "-i", str(session), "--cutoff", "0", "--symbol-path", str(symbols)],
                       stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=600)
    return session


def instruction_category(instruction):
    operands = instruction.op_str
    mnemonic = instruction.mnemonic
    if "zmm" in operands: return "AVX512"
    if "ymm" in operands: return "AVX256"
    if mnemonic.startswith("v") and "xmm" in operands:
        return "AVX scalar" if mnemonic.endswith(("sd", "ss")) else "AVX128 packed/move"
    if "xmm" in operands:
        return "SSE scalar" if mnemonic.endswith(("sd", "ss")) else "SSE128 packed/move"
    if mnemonic.startswith("j") or mnemonic in ("call", "ret", "loop"): return "control flow"
    return "integer/other"


def find_counted_loop(image, hot_rva):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for instruction in decoder.disasm(image[hot_rva:hot_rva+128], hot_rva):
        if instruction.mnemonic in ("jne", "jnz"):
            try:
                start = int(instruction.op_str, 0)
            except ValueError:
                continue
            if max(0, hot_rva-256) <= start <= hot_rva:
                finish = instruction.address + instruction.size
                body = list(decoder.disasm(image[start:finish], start))
                return {"start_rva": start, "end_rva": finish,
                        "instructions": [{"mnemonic": row.mnemonic, "operands": row.op_str} for row in body]}
    return None


def summarize(session, output):
    metadata = json.loads((session / "session.uprof").read_text(encoding="utf-8-sig"))
    log = output / (session.name + "-console.log")
    probe = record_from_log(log)
    database = duckdb.connect(str(session / "cpu.db"), read_only=True)
    events = metadata["collection"]["events"]["pmcEvents"]["pmcEventList"]
    columns = [f'event#0x{event["encodedEventId"]:x}' for event in events]
    expressions = ",".join(f'sum("{column}")' for column in columns)
    epoch = probe.get("clock_origin", {}).get("steady_epoch_nanoseconds")
    profile_epoch = metadata["collection"]["schedule"]["start"]
    # MSVC steady_clock and uProf on Windows both use QPC nanoseconds. Old records
    # lack the absolute clock; show those only with a stated approximate alignment.
    offset_ms = (epoch - profile_epoch) / 1e6 if epoch else 0.0
    if epoch and not -1000 < offset_ms < 3000:
        raise ValueError(f"Clock mismatch: {offset_ms} ms")
    def counts(where, parameters=()):
        values = database.execute("select " + expressions + " from UnifiedSampleSeries " + where, parameters).fetchone()
        return {f'{event["hwId"]:03x}:{event["mask"]:02x}': {"samples": value or 0,
                    "estimated_events": (value or 0) * event["interval"], "interval": event["interval"]}
                for event, value in zip(events, values)}
    stages = []
    for phase in probe["events"]:
        if phase["wall_ms"] < 100:
            continue
        count = counts("where windowId>=? and windowId<?", (offset_ms + phase["start_ms"], offset_ms + phase["end_ms"]))
        cycles = count.get("076:00", {}).get("estimated_events", 0)
        retired = count.get("0c0:00", {}).get("estimated_events", 0)
        branches = count.get("0c2:00", {}).get("estimated_events", 0)
        missed = count.get("0c3:00", {}).get("estimated_events", 0)
        stages.append({"name": phase["name"], "wall_ms": phase["wall_ms"], "user_ms": phase["user_ms"],
            "average_logical_cpus": phase["average_logical_cpus"], "counts": count,
            "ipc_estimate": retired / cycles if cycles else None,
            "branch_mispredict_percent_estimate": missed / branches * 100 if branches else None,
            "branch_mpki_estimate": missed / retired * 1000 if retired else None})
        shifted = counts("where windowId>=? and windowId<?",
                         (offset_ms + phase["start_ms"] + 16, offset_ms + phase["end_ms"] + 16))
        stages[-1]["alignment_sensitivity_plus_16ms"] = {
            event: {"shifted_estimated_events": shifted[event]["estimated_events"],
                    "change_percent": (shifted[event]["estimated_events"]/value["estimated_events"]-1)*100
                    if value["estimated_events"] else None} for event, value in count.items()}
    # Sampled native instruction locations. Event-based sampling has skid and is
    # not an exact dynamic opcode count. Preserve per-location counts for review.
    image_cache = {}
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    sample_column = 'event#0x10000c0'
    locations = database.execute(f'''select m.path,s.moduleRVA,sum(s."{sample_column}") n
        from UnifiedSampleSeries s join Module m on s.moduleId=m.id
        where s."{sample_column}">0 group by m.path,s.moduleRVA order by n desc''').fetchall()
    categories = defaultdict(float)
    per_location = []
    symbols = session / "matching-symbols"
    location_categories = {}
    for module, rva, weight in locations:
        path = Path(module)
        if path.suffix.lower() == ".exe":
            for base in (symbols, output / "original-symbols"):
                if (base / path.name).exists():
                    path = base / path.name
                    break
        try:
            if str(path) not in image_cache:
                image_cache[str(path)] = pefile.PE(str(path), fast_load=True).get_memory_mapped_image()
            instruction = next(decoder.disasm(image_cache[str(path)][rva:rva+16], rva), None)
            category = instruction_category(instruction) if instruction else "unresolved"
        except (OSError, pefile.PEFormatError):
            instruction, category = None, "unresolved"
        categories[category] += weight
        location_categories[(module, rva)] = category
        per_location.append({"module": module, "rva": rva, "retired_instruction_samples": weight,
            "category": category, "instruction": (instruction.mnemonic + " " + instruction.op_str) if instruction else ""})
    for phase, stage in zip((p for p in probe["events"] if p["wall_ms"] >= 100), stages):
        mix = defaultdict(float)
        for module, rva, weight in database.execute(f'''select m.path,s.moduleRVA,sum(s."{sample_column}") n
            from UnifiedSampleSeries s join Module m on s.moduleId=m.id
            where windowId>=? and windowId<? and s."{sample_column}">0
            group by m.path,s.moduleRVA''', (offset_ms + phase["start_ms"], offset_ms + phase["end_ms"])).fetchall():
            mix[location_categories[(module, rva)]] += weight
        stage["instruction_sample_categories"] = dict(mix)
        if "Function" in [row[0] for row in database.execute("show tables").fetchall()]:
            stage["top_cycle_functions"] = database.execute('''select f.name,sum(s."event#0x1000076") n
                from UnifiedSampleSeries s join Function f on s.functionId=f.id
                where windowId>=? and windowId<? group by f.name order by n desc limit 10''',
                (offset_ms + phase["start_ms"], offset_ms + phase["end_ms"])).fetchall()
        available = {row[0] for row in database.execute("describe UnifiedSampleSeries").fetchall()}
        for column, key in (("coreId", "cycle_samples_by_logical_cpu"), ("threadId", "cycle_samples_by_thread")):
            if column in available:
                distribution = database.execute(f'''select {column},sum("event#0x1000076") n
                    from UnifiedSampleSeries where windowId>=? and windowId<? group by {column}
                    having n>0 order by n desc''', (offset_ms + phase["start_ms"], offset_ms + phase["end_ms"])).fetchall()
                total = sum(row[1] for row in distribution)
                stage[key] = [{"id": identifier, "samples": amount, "percent": amount/total*100}
                              for identifier, amount in distribution] if total else []
    timeline = database.execute("select floor(windowId/100)*100 bin," + expressions +
                               " from UnifiedSampleSeries group by bin order by bin").fetchall()
    out = {"session": session.name, "clock_alignment_ms": offset_ms,
        "qpc_metadata_alignment": epoch is not None, "exact_qpc_alignment": False,
        "time_alignment_note": "QPC metadata alignment; the short calibration reveals about 16 ms of residual native-series origin offset. Per-phase +16 ms sensitivity is included.",
        "profile_duration_ms": metadata["collection"]["schedule"]["elapsed"],
        "event_definitions": events, "counts": counts(""), "stages": stages,
        "instruction_sample_categories": dict(categories), "instruction_locations": per_location,
        "timeline": timeline, "probe": probe}
    if probe.get("mode") == "flop-control":
        hot = next(row for row in per_location if row["instruction"].startswith(("addpd ", "mulpd "))
                   and row["module"].endswith(".exe"))
        path = symbols / Path(hot["module"]).name
        loop = find_counted_loop(pefile.PE(str(path), fast_load=True).get_memory_mapped_image(), hot["rva"])
        if not loop:
            raise ValueError("Cannot identify the known FP calibration loop")
        selected = [row for row in per_location if row["module"] == hot["module"]
                    and loop["start_rva"] <= row["rva"] < loop["end_rva"]]
        arithmetic = sum(row["mnemonic"] in ("mulpd", "addpd") for row in loop["instructions"])
        loop.update(retired_samples=sum(row["retired_instruction_samples"] for row in selected),
                    arithmetic_samples=sum(row["retired_instruction_samples"] for row in selected
                                           if row["instruction"].startswith(("addpd ", "mulpd "))),
                    expected_instructions=probe["stats"]["iterations"] * len(loop["instructions"]) // (arithmetic//2),
                    actual_arithmetic_percent=arithmetic/len(loop["instructions"])*100)
        out["control_loop"] = loop
    (output / (session.name + "-analysis.json")).write_text(json.dumps(out, indent=2), encoding="utf-8")
    database.close()
    print(json.dumps({"session": session.name, "qpc_offset_ms": offset_ms, "stages": len(stages), "instruction_categories": dict(categories)}), flush=True)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sets", default="branch,cache,fp")
    parser.add_argument("--mode", default="prepare")
    parser.add_argument("--analyze-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not args.analyze_only:
        for kind in args.sets.split(","):
            session = collect(args.executable.resolve(strict=True), output, kind, args.mode, args.model.resolve(strict=True))
            summarize(session, output)
    else:
        for session in output.glob("uprof-*"):
            if session.is_dir() and (session / "cpu.db").exists() and "smoke" not in session.name:
                summarize(session, output)


if __name__ == "__main__":
    main()
