"""A doctest command fixture for CTest discovery and execution checks."""
import json
from pathlib import Path
import re
import sys
import time
import uuid
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent
sys.stdout.reconfigure(encoding="utf-8")
arguments = sys.argv[1:]
cases = json.loads((ROOT / "cases.json").read_text(encoding="utf-8"))


def filters(option):
    value = next((arg.removeprefix(option + "=") for arg in arguments if arg.startswith(option + "=")), "")
    return [part.replace("\\,", ",").replace("\\\\", "\\")
            for part in re.split(r"(?<!\\),", value) if part]


def matches(value, patterns):
    return any(re.fullmatch(re.escape(pattern).replace(r"\*", ".*").replace(r"\?", "."), value)
               for pattern in patterns)


def selected(case):
    for field, option in [("filename", "--source-file"), ("name", "--test-case")]:
        include = filters(option)
        if include and not matches(case[field], include):
            return False
        if matches(case[field], filters(option + "-exclude")):
            return False
    return True


if "--list-test-cases" in arguments:
    with (ROOT / "calls.txt").open("a", encoding="utf-8") as log:
        log.write("called\n")
    if (ROOT / "query-error").exists():
        print("discovery executable failed")
        sys.exit(2)
    root = ET.Element("doctest")
    for case in cases:
        if selected(case):
            ET.SubElement(root, "TestCase", name=case["name"], filename=case["filename"],
                          line="12", skipped=str(case.get("skipped", False)).lower())
    print(ET.tostring(root, encoding="unicode"))
else:
    active = [case for case in cases if selected(case) and not case.get("skipped", False)]
    start = time.monotonic()
    if any(Path(case["filename"]).stem.endswith("_gpu_tests") for case in active):
        time.sleep(0.1)
    failures = [case for case in active if case.get("fail", False)]
    for case in failures:
        print(f'Failed case: {case["name"]} at {case["filename"]}:12')
    record = {"names": [case["name"] for case in active], "start": start, "end": time.monotonic()}
    (ROOT / "runs" / (uuid.uuid4().hex + ".json")).write_text(json.dumps(record), encoding="utf-8")
    print(f"{len(active)} cases executed; {len(failures)} cases failed")
    sys.exit(bool(failures))
