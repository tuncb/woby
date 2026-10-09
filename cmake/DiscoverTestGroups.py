"""Generate CTest source-file groups from the compiled doctest inventory."""
import argparse
from collections import Counter, defaultdict
import hashlib
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def bracket(value):
    marker = "="
    while f"]{marker}]" in value:
        marker += "="
    return f"[{marker}[{value}]{marker}]"


def filter_value(value):
    # doctest escapes comma separators and backslashes, but not wildcards.
    if "*" in value or "?" in value:
        raise ValueError(f"Cannot use an exact doctest filter for: {value}")
    return value.replace("\\", "\\\\").replace(",", "\\,")


def source_path(value):
    return Path(value.replace("\\", "/"))


def discover(command, working_directory):
    result = subprocess.run(
        [*command, "--list-test-cases", "--reporters=xml"],
        cwd=working_directory, capture_output=True, text=True, encoding="utf-8", timeout=30,
    )
    if result.returncode:
        raise ValueError(f"Test discovery failed ({result.returncode}):\n{result.stdout}{result.stderr}")
    root = ET.fromstring(result.stdout)
    cases = [dict(case.attrib) for case in root.iter("TestCase") if case.get("skipped") != "true"]
    if not cases:
        raise ValueError("Test discovery returned no active cases")
    if any(not case.get("name") or not case.get("filename") for case in cases):
        raise ValueError("Test discovery returned a case without a name or source file")
    names = [case["name"] for case in cases]
    if len(names) != len(set(names)):
        raise ValueError("Test discovery returned duplicate case names")
    return cases


def generate(target, command, working_directory, cases, slow_tests):
    missing = set(slow_tests) - {case["name"] for case in cases}
    if missing:
        raise ValueError("Slow test was not discovered: " + ", ".join(sorted(missing)))
    by_source = defaultdict(list)
    for case in cases:
        by_source[case["filename"]].append(case["name"])
    stems = Counter(source_path(source).stem for source in by_source)
    lines = []
    registered = []

    def register(name, source, arguments, slow=False):
        if name in registered:
            raise ValueError(f"Duplicate CTest name: {name}")
        gpu = source_path(source).stem.endswith("_gpu_tests")
        labels = ["unit", "graphics" if gpu else "cpu"]
        if slow:
            labels.append("slow")
        invocation = [*command, "--case-sensitive=true", "--source-file=" + filter_value(source),
                      *arguments, "--duration=true"]
        lines.append("add_test(" + " ".join(bracket(arg) for arg in [name, *invocation]) + ")")
        properties = ["WORKING_DIRECTORY", working_directory, "TIMEOUT", "180",
                      "LABELS", ";".join(labels)]
        if gpu:
            properties += ["RESOURCE_LOCK", "woby_gpu"]
        lines.append("set_tests_properties(" + bracket(name) + " PROPERTIES "
                     + " ".join(bracket(arg) for arg in properties) + ")")
        registered.append(name)

    for source, names in sorted(by_source.items()):
        slow_names = [name for name in names if name in slow_tests]
        regular_names = [name for name in names if name not in slow_tests]
        if regular_names:
            stem = source_path(source).stem
            group = f"{target}::{stem}"
            if stems[stem] > 1:
                group += "::" + hashlib.sha256(source.encode("utf-8")).hexdigest()[:8]
            arguments = []
            if slow_names:
                arguments.append("--test-case-exclude=" + ",".join(filter_value(name) for name in slow_names))
            register(group, source, arguments)
        for name in slow_names:
            register(name, source, ["--test-case=" + filter_value(name)], slow=True)

    lines.append("set(" + bracket(target + "_TESTS") + " "
                 + " ".join(bracket(name.replace(";", "\\;")) for name in registered) + ")")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True)
    parser.add_argument("--working-directory", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--slow-test", action="append", default=[])
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a test executable is required")
    try:
        cases = discover(command, args.working_directory)
        content = generate(args.target, command, args.working_directory, cases, set(args.slow_test))
    except (ValueError, ET.ParseError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Grouped test discovery: {error}\n")
    # Commit the cache only after discovery and classification both succeed.
    temporary = args.output.with_suffix(".tmp")
    temporary.write_text(content, encoding="utf-8")
    temporary.replace(args.output)


if __name__ == "__main__":
    main()
