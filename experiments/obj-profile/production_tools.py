# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Preserve and inspect production executable code without recompiling it."""
import argparse
from bisect import bisect_right
from collections import defaultdict
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil

import pefile
import capstone
import duckdb


def map_functions(text, image_base):
    functions = defaultdict(list)
    for line in text.splitlines():
        match = re.match(r"\s+0001:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\s+(.*)$", line)
        if match:
            name, address, object_name = match.groups()
            functions[int(address, 16) - image_base].append((name, object_name.strip()))
    return dict(sorted(functions.items()))


def nearest_function(addresses, rva):
    index = bisect_right(addresses, rva) - 1
    return addresses[index] if index >= 0 else None


def undecorate(name):
    function = ctypes.WinDLL("dbghelp").UnDecorateSymbolName
    function.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32)
    function.restype = ctypes.c_uint32
    buffer = ctypes.create_string_buffer(16384)
    return buffer.value.decode(errors="replace") if function(name.encode(), buffer, len(buffer), 0) else name


def analyze_production(exe, map_path, session, viewer_path):
    metadata = json.loads((session / "session.uprof").read_text(encoding="utf-8-sig"))
    viewer = json.loads(viewer_path.read_text(encoding="utf-8"))
    binary = pefile.PE(str(exe), fast_load=True)
    image = binary.get_memory_mapped_image()
    functions = map_functions(map_path.read_text(encoding="utf-8"), binary.OPTIONAL_HEADER.ImageBase)
    addresses = list(functions)
    profile_epoch = metadata["collection"]["schedule"]["start"]
    offset_ms = (viewer["observer_epoch_qpc_ns"] - profile_epoch) / 1e6
    begin = offset_ms + viewer["load_request_ms"]
    finish = begin + viewer["load_rpc_ms"]
    database = duckdb.connect(str(session / "cpu.db"), read_only=True)
    rows = database.execute('''select m.path,s.moduleRVA,sum(s."event#0x1000076") cycles,
        sum(s."event#0x10000c0") retired from UnifiedSampleSeries s join Module m on s.moduleId=m.id
        where windowId>=? and windowId<? group by m.path,s.moduleRVA''', [begin, finish]).fetchall()
    grouped = defaultdict(lambda: [0.0, 0.0])
    for module, rva, cycles, retired in rows:
        address = nearest_function(addresses, rva) if Path(module).name.lower() == exe.name.lower() else None
        key = address if address is not None else str(module)
        grouped[key][0] += cycles or 0
        grouped[key][1] += retired or 0
    total_cycles = sum(value[0] for value in grouped.values())
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    output = []
    for key, (cycles, retired) in sorted(grouped.items(), key=lambda item: item[1][0], reverse=True):
        names = [undecorate(name) for name, obj in functions[key]] if isinstance(key, int) else [key]
        instructions = []
        if isinstance(key, int) and any("buildObjMesh" in name and "lambda_2" in name for name in names):
            for instruction in decoder.disasm(image[key:key+96], key):
                instructions.append(f"{instruction.address:x}: {instruction.mnemonic} {instruction.op_str}")
                if instruction.mnemonic == "ret":
                    break
        output.append({"rva": key if isinstance(key, int) else None, "aliases": names,
                       "cycle_samples": cycles, "retired_samples": retired,
                       "cycle_sample_percent": cycles / total_cycles * 100,
                       "instructions": instructions})
    events = metadata["collection"]["events"]["pmcEvents"]["pmcEventList"]
    count = {}
    for event in events:
        value = database.execute(f'''select sum("event#0x{event['encodedEventId']:x}")
             from UnifiedSampleSeries where windowId>=? and windowId<?''', [begin, finish]).fetchone()[0] or 0
        count[f"{event['hwId']:03x}:{event['mask']:02x}"] = {"samples": value, "interval": event["interval"],
                                                            "estimated_events": value * event["interval"]}
    database.close()
    return {"exact_code_sha256": code_hash(exe)["section_sha256"][".text"], "clock_offset_ms": offset_ms,
            "load_window_ms": [begin, finish], "counts": count, "functions": output,
            "resolution": "Nearest CODE function start from linker map; identical-code aliases are preserved"}


def code_hash(path):
    binary = pefile.PE(str(path), fast_load=True)
    sections = {section.Name.decode().rstrip("\0"): hashlib.sha256(section.get_data()).hexdigest()
                for section in binary.sections}
    return {"path": str(path), "file_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "section_sha256": sections, "image_base": binary.OPTIONAL_HEADER.ImageBase}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--map", type=Path)
    parser.add_argument("--session", type=Path)
    parser.add_argument("--viewer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    exe = args.executable.resolve(strict=True)
    result = code_hash(exe)
    if args.snapshot:
        args.snapshot.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe, args.snapshot / exe.name)
    if args.compare:
        before = code_hash(args.compare.resolve(strict=True))
        result = {"before": before, "after": result,
                  "text_identical": before["section_sha256"][".text"] == result["section_sha256"][".text"]}
        if not result["text_identical"]:
            raise RuntimeError("Production .text changed while adding a linker map")
    if args.session:
        if not args.map or not args.viewer:
            parser.error("--session requires --map and --viewer")
        result = analyze_production(exe, args.map, args.session, args.viewer)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result if not args.session else {"clock_offset_ms": result["clock_offset_ms"],
                    "functions": result["functions"][:8]}, indent=2))


if __name__ == "__main__":
    main()
