# /// script
# requires-python = ">=3.11"
# dependencies = ["pefile>=2024", "capstone>=5", "duckdb>=1.3"]
# ///
"""Preserve the compiled quality stop-check and representative sort code."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from bisect import bisect_right

import capstone
import pefile


def audit(root):
    source = Path(__file__).resolve().parents[1] / "obj-profile/production_tools.py"
    spec = importlib.util.spec_from_file_location("production_tools",source)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    result = {"scope":"Static executable code. Linker-map boundaries can include padding. This does not count executed instructions.","binaries":[]}
    for name in ("bus-token-live","bus-token-reference","bus-max3-control","bus-max3"):
        if not (root / name / "metadata.json").exists():
            continue
        metadata = json.loads((root / name / "metadata.json").read_text())
        directory = Path(metadata["symbols"]); executable = next(directory.glob("*.exe"))
        digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        if digest != metadata["executable_sha256"]:
            raise ValueError("Executable hash mismatch")
        pe = pefile.PE(str(executable),fast_load=True); binary = pe.get_memory_mapped_image()
        functions = module.map_functions(next(directory.glob("*.map")).read_text(),pe.OPTIONAL_HEADER.ImageBase)
        addresses = list(functions); rows = []
        decoder = capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
        for index,address in enumerate(addresses):
            aliases = functions[address]
            if not any(("surface_mesh_quality" in obj and ("canceled@" in symbol
                    or ("statistics@" in symbol and "_Partition_" in symbol)))
                    or ("max3" in name and "mesh_comparison" in obj and ("degenerate@" in symbol or "triangleSquared@" in symbol))
                    for symbol,obj in aliases):
                continue
            end = min(address+4096,addresses[index+1] if index+1 < len(addresses) else len(binary))
            instructions = [{"rva":i.address,"bytes":i.bytes.hex(),"text":f"{i.mnemonic} {i.op_str}"}
                for i in decoder.disasm(binary[address:end],address)]
            calls = []
            for instruction in instructions:
                if instruction["text"].startswith("call 0x"):
                    target = int(instruction["text"].split()[1],16)
                    target_index = bisect_right(addresses,target)-1
                    names = functions.get(addresses[target_index],[]) if target_index >= 0 else []
                    calls.append({"rva":instruction["rva"],"target":target,
                        "nearest_aliases":[module.undecorate(n) for n,o in names]})
            rows.append({"rva":address,"map_end":end,"aliases":[{"name":module.undecorate(n),"object":o} for n,o in aliases],
                "instructions":instructions,"direct_calls":calls,
                "locked_instructions":[i for i in instructions if i["text"].startswith("lock ")]})
        result["binaries"].append({"case":name,"sha256":digest,"symbols":str(directory),"functions":rows})
        print(json.dumps({"case":name,"functions":len(rows),"locked_instructions":sum(len(r["locked_instructions"]) for r in rows)}))
    (root / "code-audit.json").write_text(json.dumps(result,indent=2),encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("root",type=Path)
    audit(parser.parse_args().root.resolve())
