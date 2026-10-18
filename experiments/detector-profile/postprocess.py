"""Process retained captures after all measured workloads have stopped."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root",type=Path); parser.add_argument("destination",type=Path)
    parser.add_argument("--steps",default="hardware,power,vtune,code,verify,evidence,plots")
    args = parser.parse_args(); root = args.root.resolve(); destination = args.destination.resolve()
    scripts = Path(__file__).resolve().parent; steps = args.steps.split(","); failures = []
    directories = sorted((d for d in root.iterdir() if (d / "metadata.json").exists()),
        key=lambda d:(not d.name.startswith("bus-"),d.name))
    def command(argv):
        result = subprocess.run(argv)
        if result.returncode:
            failures.append({"command":list(map(str,argv)),"exit_code":result.returncode})
        return result.returncode == 0
    for directory in directories:
        metadata = read(directory / "metadata.json")
        if metadata.get("exit_code") != 0 or not (directory / "probe.json").exists():
            continue
        if "hardware" in steps and (directory / "session/cpu.db").exists():
            digest = hashlib.sha256((scripts / "hardware.py").read_bytes()).hexdigest()
            prior = directory / "hardware.json"
            if not prior.exists() or read(prior).get("analyzer_sha256") != digest:
                command(["uv","run",str(scripts / "hardware.py"),str(directory)])
        if "power" in steps and metadata["profile"] == "timechart":
            command([sys.executable,str(scripts / "power.py"),str(directory)])
        if "vtune" in steps and metadata["profile"] == "vtune":
            executable = metadata["command"][0]
            for report,suffix,extra in (("summary","txt",[]),("hotspots","csv",["-format","csv","-csv-delimiter","comma","-limit","1000"])):
                path = directory / ("vtune-"+report+"."+suffix)
                command([executable,"-report",report,"-r",str(directory / "vtune"),"-report-output",str(path),*extra])
    if "code" in steps:
        command(["uv","run",str(scripts / "code_audit.py"),str(root)])
    if "verify" in steps:
        command([sys.executable,str(scripts / "verify_results.py"),str(root)])
    if "evidence" in steps:
        command([sys.executable,str(scripts / "evidence.py"),str(root),str(destination)])
    if "plots" in steps:
        command(["uv","run",str(scripts / "plots.py"),str(destination)])
        command([sys.executable,str(scripts / "dashboard.py"),str(destination)])
    (root / "postprocess.json").write_text(json.dumps({"steps":steps,"failures":failures},indent=2),encoding="utf-8")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
