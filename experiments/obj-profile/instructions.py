# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone>=5", "pefile>=2024.8.26"]
# ///
"""Decode sampled instruction addresses and summarize generated assembly.

Counts are code locations or biased time samples, never retired instructions.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

import capstone
import pefile


def category(mnemonic, operands):
    if "zmm" in operands or mnemonic.startswith("v") and "{k" in operands:
        return "AVX512"
    if mnemonic.startswith("v") and mnemonic not in ("verr", "verw"):
        if "ymm" in operands:
            return "AVX_256"
        if mnemonic.endswith(("ss", "sd")) and mnemonic.startswith(("vadd", "vsub", "vmul", "vdiv", "vsqrt", "vcvt", "vcom", "vucom", "vcmp", "vmin", "vmax", "vfm")):
            return "AVX_scalar_FP"
        return "AVX_128_or_control"
    if "xmm" in operands:
        if mnemonic.endswith(("ss", "sd")) and mnemonic.startswith(("add", "sub", "mul", "div", "sqrt", "cvt", "com", "ucom", "cmp", "min", "max")):
            return "SSE_scalar_FP"
        if mnemonic.startswith(("mov", "cvt", "unpck", "shuf", "xor")):
            return "SSE_move_convert_shuffle"
        return "SSE_packed"
    if mnemonic == "rep" or mnemonic.startswith("rep "):
        return "REP_memory"
    return "integer_branch_other"


def assembly(path):
    functions = {}
    name = None
    counts = Counter()
    examples = defaultdict(set)
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^(\S+)\s+PROC\b", line)
        if match:
            name, counts, examples = match[1], Counter(), defaultdict(set)
        if name and re.match(r"^\S+\s+ENDP\b", line):
            if sum(counts.values()):
                functions[name] = {"instruction_locations": dict(counts),
                                   "examples": {key: sorted(value) for key, value in examples.items()}}
            name = None
        if name:
            match = re.match(r"^\s+([a-z][a-z0-9]+)\s+([^;]*)", line)
            if match and match[1] not in {"db", "dd", "dq", "dw", "align", "npad"}:
                group = category(match[1], match[2])
                counts[group] += 1
                if group != "integer_branch_other":
                    examples[group].add(match[1])
    return functions


def samples(records):
    files = {}
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    by_phase = defaultdict(Counter)
    by_symbol = defaultdict(Counter)
    decoded = []
    for record in records:
        for row in record.get("instruction_samples", []):
            module = row["module"]
            if module not in files:
                try:
                    files[module] = pefile.PE(module, fast_load=True)
                except (OSError, pefile.PEFormatError):
                    files[module] = None
            pe = files[module]
            instruction = None
            if pe:
                try:
                    instruction = next(decoder.disasm(pe.get_data(row["rva"], 16), row["rva"], 1), None)
                except (pefile.PEFormatError, ValueError):
                    pass
            group = category(instruction.mnemonic, instruction.op_str) if instruction else "undecoded"
            by_phase[row["phase"]][group] += row["count"]
            symbol = row["symbol"] or Path(module).name
            by_symbol[row["phase"]][symbol] += row["count"]
            if instruction:
                decoded.append(dict(row, mnemonic=instruction.mnemonic, operands=instruction.op_str, category=group))
    return {"sample_categories": {key: dict(value) for key, value in by_phase.items()},
            "hot_symbols": {key: value.most_common(15) for key, value in by_symbol.items()}, "decoded_samples": decoded}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, nargs="*", default=[])
    parser.add_argument("--assembly", type=Path, nargs="*", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = {"method": "Static compiler assembly plus instruction pointer samples filtered by thread CPU progress. These are not dynamic instruction counts.",
              "assembly": {str(path): assembly(path) for path in args.assembly}}
    records = [record for path in args.samples for record in json.loads(path.read_text(encoding="utf-8"))["records"]]
    output.update(samples(records))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(json.dumps({"hot_symbols": output["hot_symbols"], "sample_categories": output["sample_categories"]}, indent=2))


if __name__ == "__main__":
    main()
