# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Inspect parser sample locations and paired compiler experiments.

An event sample identifies an instruction location. It is not an exact opcode
count or an unbiased estimate of the fraction of executed SIMD instructions.
"""
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys

import capstone
import duckdb
import pefile


SCALAR_FP = ("add", "sub", "mul", "div", "sqrt", "cvt", "com", "ucom", "cmp", "min", "max", "fm")
FUNCTIONS = {
    "Decimal conversion": "??$from_chars_advanced@N@",
    "General face references": "?ParseFace@",
    "Position-only face probe": "?PrototypeSimpleFace@",
    "Position records": "?PrototypePosition@",
    "UV records": "??$PrototypeAttribute@$0A@",
    "Index merge / validation": "?Execute@CopyIndices@",
    "Line boundaries": "?PrototypeCompleteLines@",
}


def opcode_kind(mnemonic, operands):
    """Separate calculation from transfers and register setup."""
    mnemonic = mnemonic.lower()
    operands = operands.lower()
    operation = mnemonic[1:] if mnemonic.startswith("v") else mnemonic
    vector = any(register in operands for register in ("xmm", "ymm", "zmm"))
    if not vector:
        return "Integer / control / other"
    if operation.endswith(("ss", "sd")) and operation.startswith(SCALAR_FP):
        return "Scalar FP calculation / compare"
    if operation.startswith(("mov", "maskmov", "broadcast", "insert", "extract", "perm", "shuf", "unpck", "punpck", "blend", "pack")):
        return "SIMD-register moves / setup"
    if operation.startswith(("xor", "pxor", "and", "pand", "or", "por")):
        parts = [part.strip() for part in operands.split(",")]
        if "xor" in operation and len(parts) >= 2 and len(set(parts)) == 1:
            return "SIMD-register moves / setup"
        return "Vector logical / mask"
    if operation in ("pmovmskb", "movmskps", "movmskpd"):
        return "Vector logical / mask"
    if operation.startswith(("padd", "psub", "pmul", "pcmp", "pmin", "pmax", "psll", "psrl", "psra", "ptest", "pabs", "psad")):
        return "Packed calculation / compare"
    if operation.endswith(("ps", "pd")) and operation.startswith(SCALAR_FP):
        return "Packed calculation / compare"
    if operation.startswith("cvt"):
        return "Packed calculation / compare"
    return "Other SIMD-register operation"


def encoding(mnemonic, operands):
    if "zmm" in operands:
        return "EVEX / 512-bit"
    if "ymm" in operands:
        return "VEX / 256-bit"
    if "xmm" in operands:
        return "VEX / 128-bit register" if mnemonic.startswith("v") else "Legacy / 128-bit register"
    if mnemonic in ("shlx", "shrx", "sarx", "bzhi", "pdep", "pext", "mulx", "rorx"):
        return "BMI2 scalar integer"
    if mnemonic in ("andn", "bextr", "blsi", "blsmsk", "blsr", "tzcnt"):
        return "BMI1 scalar integer"
    return "Scalar / control"


def assembly_inventory(text):
    """Count static instruction locations only within selected PROC blocks."""
    inventories = {}
    current = None
    for line in text.splitlines():
        start = re.match(r"^(\S+)\s+PROC\b", line)
        if start:
            current = next((name for name, pattern in FUNCTIONS.items() if start[1].startswith(pattern)), None)
            if current:
                inventories[current] = {"symbol": start[1], "instructions": []}
        if re.match(r"^\S+\s+ENDP\b", line):
            current = None
        instruction = re.match(r"^\s+([a-z][a-z0-9]+)\s+([^;]*)", line)
        if current and instruction and instruction[1] not in {"db", "dd", "dq", "dw", "align", "npad"}:
            mnemonic, operands = instruction[1], instruction[2].strip()
            inventories[current]["instructions"].append({"mnemonic": mnemonic, "operands": operands,
                "kind": opcode_kind(mnemonic, operands), "encoding": encoding(mnemonic, operands)})
    for values in inventories.values():
        values["static_instruction_locations"] = len(values["instructions"])
        values["kinds"] = dict(Counter(row["kind"] for row in values["instructions"]))
        values["encodings"] = dict(Counter(row["encoding"] for row in values["instructions"]))
        values["mnemonics"] = dict(Counter(row["mnemonic"] for row in values["instructions"]))
    return inventories


def distribution(values):
    return {"n": len(values), "median": statistics.median(values), "min": min(values), "max": max(values), "values": values}


def comparison(records):
    groups = defaultdict(list)
    for record in records:
        if record.get("instruction_samples"):
            raise ValueError("Sampled runs cannot enter timing medians")
        phase = next(row for row in record["events"] if row["name"] == "parser_total")
        groups[(record["compiler_isa"], record["workers_requested"])].append((record, phase))
    result = []
    for (isa, workers), rows in sorted(groups.items(), key=lambda item: (item[0][1], item[0][0])):
        result.append({"isa": isa, "workers": workers,
            "wall_ms": distribution([phase["wall_ms"] for _, phase in rows]),
            "cpu_ms": distribution([phase["cpu_ms"] for _, phase in rows]),
            "logical_cpus": distribution([phase["average_logical_cpus"] for _, phase in rows]),
            "scan_ms": distribution([next(p["wall_ms"] for p in row["events"] if p["name"] == "parser_scan_collect") for row, _ in rows]),
            "merge_ms": distribution([next(p["wall_ms"] for p in row["events"] if p["name"] == "parser_merge") for row, _ in rows]),
            "rounds": [row["round"] for row, _ in rows]})
    fingerprints = {row["geometry"]["fingerprint"] for row in records}
    if len(fingerprints) != 1:
        raise ValueError("Parser output fingerprints differ")
    return {"groups": result, "runs": len(records), "fingerprint": next(iter(fingerprints))}


def sample_rows(database, begin, end, process_id, thread_ids):
    placeholders = ",".join("?" for _ in thread_ids)
    # Runtime instructions can have no matching Function row. Keep their module
    # and address so that a missing name does not discard vector copy evidence.
    return database.execute(f'''select m.path,coalesce(f.name,'[unresolved function]'),s.moduleRVA,
            sum(s."event#0x1000076") cycles,sum(s."event#0x10000c0") retired
        from UnifiedSampleSeries s join Module m on s.moduleId=m.id left join Function f on s.functionId=f.id
        where s.windowId>=? and s.windowId<? and s.processId=? and s.threadId in ({placeholders})
        group by m.path,f.name,s.moduleRVA having cycles>0 or retired>0''', [begin, end, process_id, *thread_ids]).fetchall()


def parser_samples(capture):
    branch = capture / "deep/branch"
    session = branch / "cpu"
    record = json.loads((branch / "result.json").read_text(encoding="utf-8"))
    metadata = json.loads((session / "session.uprof").read_text(encoding="utf-8-sig"))
    origin = next(row["steady_epoch_nanoseconds"] for row in record["trace"] if row["kind"] == "metadata")
    offset = (origin - metadata["collection"]["schedule"]["start"]) / 1e6
    database = duckdb.connect(str(session / "cpu.db"), read_only=True)
    process_id = record["pid"]
    sample_processes = {row[0] for row in database.execute("select distinct processId from UnifiedSampleSeries").fetchall()}
    if process_id not in sample_processes:
        raise ValueError("Native process ID is not in the sample table")
    sample_ids = {row[0] for row in database.execute("select distinct threadId from UnifiedSampleSeries").fetchall()}
    worker_ids = {row["thread_id"] for row in record["trace"] if row["kind"] == "worker"}
    if not worker_ids.issubset(sample_ids):
        raise ValueError("Native parser worker IDs are not in the sample table")
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    images = {}
    scopes = []
    for phase_name in ("parser_scan_collect", "parser_merge"):
        phase = next(row for row in record["trace"] if row["kind"] == "phase" and row["name"] == phase_name)
        begin, end = offset + phase["start_ms"], offset + phase["end_ms"]
        if phase_name == "parser_scan_collect":
            selected_threads = sorted(worker_ids)
            scope_note = "Only the 16 native parser read/decode workers; renderer and coordinator excluded."
        else:
            selected_threads = sorted({row[0] for row in database.execute('''
                select distinct s.threadId from UnifiedSampleSeries s join Function f on s.functionId=f.id
                where s.windowId>=? and s.windowId<? and s.processId=? and s."event#0x1000076">0
                and f.name like '%rapidobj::%' ''', [begin, end, process_id]).fetchall()} | {phase["thread_id"]})
            scope_note = "Threads with rapidobj cycle samples during merge, including its coordinator and merge workers."
        if not selected_threads:
            raise ValueError("No parser threads for " + phase_name)
        rows = sample_rows(database, begin, end, process_id, selected_threads)
        functions = {}
        locations = []
        for module, name, rva, cycles, retired in rows:
            path = Path(module)
            if path.suffix.lower() == ".exe":
                path = session / "matching-symbols" / path.name
            if str(path) not in images:
                try:
                    images[str(path)] = pefile.PE(str(path), fast_load=True).get_memory_mapped_image()
                except (OSError, pefile.PEFormatError):
                    images[str(path)] = None
            instruction = next(decoder.disasm(images[str(path)][rva:rva + 16], rva, 1), None) if images[str(path)] else None
            mnemonic, operands = (instruction.mnemonic, instruction.op_str) if instruction else ("", "")
            kind = opcode_kind(mnemonic, operands) if instruction else "Unresolved"
            row = {"module": module, "function": name, "rva": rva, "rva_hex": hex(rva),
                "cycle_samples": cycles or 0, "retired_event_samples": retired or 0,
                "mnemonic": mnemonic, "operands": operands, "kind": kind,
                "encoding": encoding(mnemonic, operands) if instruction else "Unresolved"}
            locations.append(row)
            key = (module, name)
            if key not in functions:
                functions[key] = {"module": module, "function": name, "cycle_samples": 0, "retired_event_samples": 0,
                    "kinds": Counter(), "encodings": Counter(), "mnemonics": Counter()}
            function = functions[key]
            function["cycle_samples"] += cycles or 0
            function["retired_event_samples"] += retired or 0
            function["kinds"][kind] += retired or 0
            function["encodings"][row["encoding"]] += retired or 0
            function["mnemonics"][mnemonic] += retired or 0
        total_cycles = sum(row["cycle_samples"] for row in functions.values())
        scopes.append({"phase": phase_name, "scope_note": scope_note, "thread_ids": selected_threads,
            "wall_ms": phase["wall_ms"], "cycle_samples": total_cycles,
            "functions": sorted(functions.values(), key=lambda row: -row["cycle_samples"]),
            "locations": sorted(locations, key=lambda row: -row["cycle_samples"])})
    database.close()
    exe = session / "matching-symbols/woby_obj_e2e_viewer.exe"
    exe_digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    if exe_digest != record["executable_sha256"]:
        raise ValueError("Parser samples do not have their matching executable")
    return {"source": str(session), "executable_sha256": exe_digest,
        "clock_alignment_ms": offset, "alignment_note": "QPC metadata alignment; sampling skid and phase-boundary uncertainty remain.",
        "scopes": scopes}


def context_samples(path):
    records = json.loads(path.read_text(encoding="utf-8"))["records"]
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    images = {}
    result = []
    for record in records:
        locations = []
        for row in record.get("instruction_samples", []):
            if not row["phase"].startswith("parser_"):
                continue
            module = row["module"]
            if module not in images:
                images[module] = pefile.PE(module, fast_load=True).get_memory_mapped_image()
            instruction = next(decoder.disasm(images[module][row["rva"]:row["rva"] + 16], row["rva"], 1), None)
            if instruction:
                locations.append(dict(row, mnemonic=instruction.mnemonic, operands=instruction.op_str,
                    kind=opcode_kind(instruction.mnemonic, instruction.op_str),
                    encoding=encoding(instruction.mnemonic, instruction.op_str)))
        result.append({"isa": record["compiler_isa"], "locations": locations,
            "note": "CPU-progress-filtered thread context samples; sparse and biased. Excluded from timing medians. Runtime names can be nearest exports, so use module/RVA/opcode."})
    return result


def make(capture):
    output = capture / "parser-simd"
    measurement = json.loads((output / "isa-comparison.json").read_text(encoding="utf-8"))
    for path, digest in measurement["executable_sha256"].items():
        if hashlib.sha256(Path(path).read_bytes()).hexdigest() != digest:
            raise ValueError("Compiler-comparison executable changed: " + path)
    result = {"compiler_comparison": comparison(measurement["records"]),
        "comparison_method": measurement["method"], "comparison_provenance": {key: value for key, value in measurement.items() if key != "records"},
        "compiler_scope": "The parser headers and harness translation units differ by the compiler ISA setting. Linked woby_core and CPU-dispatched runtime helpers remain common. This is not an all-code SSE2 deployment test.",
        "static_inventory": {}, "native_samples": parser_samples(capture)}
    for isa in ("sse2", "avx2"):
        path = output / (isa + "-main.asm")
        result["static_inventory"][isa] = {"sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "functions": assembly_inventory(path.read_text(encoding="utf-8", errors="replace"))}
    contexts = output / "context-samples.json"
    if contexts.exists():
        result["context_samples"] = context_samples(contexts)
    (output / "analysis.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"saved": str(output / "analysis.json"), "comparison": result["compiler_comparison"],
        "scopes": [{k: row[k] for k in ("phase", "cycle_samples", "scope_note")} for row in result["native_samples"]["scopes"]]}))


if __name__ == "__main__":
    make(Path(sys.argv[1]).resolve(strict=True))
