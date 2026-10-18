# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6"]
# ///
"""Record host topology, model hashes, and the installed AMD event definitions."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

from run import SETS, UPROF, write


def record_events(output):
    event_file = UPROF.parent / "Data/Events/0x19_0x5.xml"
    shutil.copy2(event_file,output / event_file.name)
    events = ET.parse(event_file).getroot()
    selected = {}
    for kind, definitions in SETS.items():
        selected[kind] = []
        for value, mask, interval in definitions:
            matches = [e for e in events.iter("event") if int(e.attrib["value"],16) == value]
            selected[kind].append({"event": f"{value:03x}","mask": f"{mask:02x}","interval": interval,
                "definition": ET.tostring(matches[0],encoding="unicode") if matches else None})
    write(output / "event-definitions.json", selected)


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("output",type=Path)
    parser.add_argument("--events-only",action="store_true")
    args = parser.parse_args(); output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    record_events(output)
    if args.events_only:
        return
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("micro_run", root / "experiments/loading-audit/micro_run.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    write(output / "topology.json", module.topology())
    models = []
    for path in sorted(Path("D:/temp/obj_tests").glob("*.obj")):
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream,"sha256").hexdigest()
        models.append({"path": str(path), "bytes": path.stat().st_size, "sha256": digest})
        print(path.name, digest, flush=True)
    write(output / "models.json", models)
    commands = {
        "uprof-version": [str(UPROF), "--version"],
        "uprof-system": [str(UPROF), "info", "--system"],
        "wpr-sources": ["wpr", "-pmcsources"],
        "wpr-status": ["wpr", "-status"],
        "power-plan": ["powercfg", "/getactivescheme"],
    }
    for name, command in commands.items():
        try:
            result = subprocess.run(command,input="\n",text=True,capture_output=True,timeout=20)
            write(output / (name+".json"), {"command":command,"exit_code":result.returncode,"stdout":result.stdout,"stderr":result.stderr})
        except subprocess.TimeoutExpired as error:
            write(output / (name+".json"), {"command":command,"error":"timeout", "stdout": (error.stdout or b"").decode(errors="replace")})
    write(output / "host.json", {"platform":platform.platform(),"python":sys.version,
        "commit":subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip()})


if __name__ == "__main__":
    main()
