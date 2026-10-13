# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Measure corner mapping with fresh processes and preserve each probe result."""
import argparse
import hashlib
import json
import math
import os
import platform
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


SERIAL_VARIANTS = {"baseline", "pointer"}
DEFAULT_VARIANTS = "baseline,pointer,split,dense,compact,release,compact_release,fast,fast_release"
ROOT = Path(__file__).resolve().parents[2]


def parse_workers(value):
    try:
        workers = [int(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("Workers must be comma-separated integers.") from error
    if not workers or len(set(workers)) != len(workers) or any(item < 1 or item > 32 for item in workers):
        raise argparse.ArgumentTypeError("Use distinct gather worker counts from 1 to 32.")
    return workers


def configuration_key(configuration):
    return (f"{configuration['variant']}-p{configuration['parser_workers']}"
            f"-g{configuration['gather_workers']}-{configuration['preparation']}"
            f"-u{int(configuration.get('unique_uv_validation', True))}")


def configurations_for(variants, parser_workers, gather_workers, preparations, unique_uv_modes=None):
    if not variants or len(set(variants)) != len(variants):
        raise ValueError("Use at least one distinct variant.")
    if (not preparations or len(set(preparations)) != len(preparations)
            or any(item not in {"deferred", "eager"} for item in preparations)):
        raise ValueError("Use distinct preparation modes: deferred or eager.")
    if not gather_workers or any(item < 1 or item > 32 for item in gather_workers):
        raise ValueError("Use 1 to 32 gather workers.")
    uv_modes = [True] if unique_uv_modes is None else unique_uv_modes
    if not uv_modes or len(set(uv_modes)) != len(uv_modes) or any(not isinstance(item, bool) for item in uv_modes):
        raise ValueError("Use distinct unique UV validation modes.")
    result = []
    for preparation in preparations:
        for variant in variants:
            workers = [1] if variant in SERIAL_VARIANTS else gather_workers
            for gather_count in workers:
                for unique_uv_validation in uv_modes:
                    result.append({"variant": variant, "parser_workers": parser_workers,
                                   "gather_workers": gather_count, "preparation": preparation,
                                   "unique_uv_validation": unique_uv_validation})
    return result


def rotated_order(configurations, round_index):
    if not configurations:
        raise ValueError("There are no configurations to run.")
    shift = round_index % len(configurations)
    return configurations[shift:] + configurations[:shift]


def canonical_geometry(probe):
    """Compare ordered output, including normal and source fingerprints."""
    geometry = probe.get("geometry")
    if not isinstance(geometry, dict) or not geometry:
        raise ValueError("The probe did not report geometry.")
    return {key: value for key, value in geometry.items()
            if key != "retained_buffer_capacity_bytes"}


def validate_probe(probe, configuration):
    for output_key, config_key in (("variant", "variant"),
                                   ("parser_workers_requested", "parser_workers"),
                                   ("gather_workers_requested", "gather_workers")):
        if probe.get(output_key) != configuration[config_key]:
            raise ValueError(f"Probe {output_key} does not match the requested configuration.")
    if probe.get("deferred") is not (configuration["preparation"] == "deferred"):
        raise ValueError("Probe preparation does not match the requested configuration.")
    if "unique_uv_validation" in configuration and probe.get("unique_uv_validation") is not configuration["unique_uv_validation"]:
        raise ValueError("Probe unique UV validation does not match the requested configuration.")
    total = probe.get("cpu_load_ms")
    if not isinstance(total, (int, float)) or not math.isfinite(total) or total < 0:
        raise ValueError("The probe did not report a valid CPU load time.")
    canonical_geometry(probe)
    if not isinstance(probe.get("events"), list):
        raise ValueError("The probe did not report measurement events.")
    for event in probe["events"]:
        if not isinstance(event.get("name"), str):
            raise ValueError("An event has no name.")
        for field in ("wall_ms", "cpu_ms"):
            value = event.get(field)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError(f"Event {event['name']} has invalid {field}.")


def warm_file(path):
    with path.open("rb") as source:
        while source.read(8 * 1024 * 1024):
            pass


def file_identity(path):
    with path.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest}


def source_identities(root=ROOT):
    paths = set((root / "experiments/corner-mapping").glob("*"))
    paths.add(root / "CMakeLists.txt")
    paths.update((root / "build/vs2026-vcpkg/experiments/corner-mapping/generated").rglob("*"))
    paths.update((root / "experiments/obj-profile" / name) for name in ("profile.cpp", "profile.h", "generate.py"))
    paths.update((root / "src" / name) for name in
                 ("obj_mesh.cpp", "obj_mesh.h", "obj_freeform.cpp", "obj_freeform.h",
                  "model_mesh.cpp", "model_mesh.h", "model_load.cpp", "model_load.h",
                  "freeform.cpp", "freeform.h"))
    paths.update((root / "third_party/rapidobj/include/rapidobj").glob("*.hpp"))
    return {path.relative_to(root).as_posix(): file_identity(path)
            for path in sorted(paths) if path.is_file()}


def numeric_leaves(value, prefix=""):
    if isinstance(value, dict):
        for key, child in value.items():
            yield from numeric_leaves(child, f"{prefix}.{key}" if prefix else key)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from numeric_leaves(child, f"{prefix}.{index}")
    elif isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
        yield prefix, value


def stage_wall_ms(probe, *names):
    return sum(event["wall_ms"] for event in probe.get("events", []) if event["name"] in names)


def summarize(records):
    grouped = {}
    for record in records:
        grouped.setdefault(configuration_key(record["configuration"]), []).append(record)
    summary = {}
    for key, rows in grouped.items():
        event_values, data_values, memory_values = {}, {}, {}
        for row in rows:
            process_events = {}
            for event in row["probe"].get("events", []):
                for field in ("wall_ms", "cpu_ms", "working_set_begin", "working_set_end",
                              "commit_begin", "commit_end", "process_peak_working_set", "process_peak_commit"):
                    if field in event:
                        event_key = f"{event['name']}.{field}"
                        if field in {"wall_ms", "cpu_ms"}:
                            process_events[event_key] = process_events.get(event_key, 0) + event[field]
                        else:
                            process_events[event_key] = max(process_events.get(event_key, 0), event[field])
                for field, value in numeric_leaves(event.get("data", {}), event["name"]):
                    data_values.setdefault(field, []).append(value)
            for field, value in process_events.items():
                event_values.setdefault(field, []).append(value)
            for field, value in numeric_leaves(row["probe"].get("memory", {})):
                memory_values.setdefault(field, []).append(value)
        summary[key] = {"runs": len(rows),
                        "cpu_load_median_ms": statistics.median(row["probe"]["cpu_load_ms"] for row in rows),
                        "process_wall_median_ms": statistics.median(row["process_wall_ms"] for row in rows),
                        "mapping_pipeline_median_ms": statistics.median(
                            stage_wall_ms(row["probe"], "mesh_allocation", "corner_mapping") for row in rows),
                        "event_medians": {field: statistics.median(values) for field, values in event_values.items()},
                        "memory_data": {
                            field: {"median": statistics.median(values), "min": min(values), "max": max(values)}
                            for field, values in memory_values.items()},
                        "pass_and_buffer_data": {
                            field: {"median": statistics.median(values), "min": min(values), "max": max(values)}
                            for field, values in data_values.items()}}
    paired = {}
    for key, rows in grouped.items():
        ratios, mapping_ratios = [], []
        for row in rows:
            config = row["configuration"]
            baseline = next((other for other in records if other["round"] == row["round"]
                             and other["configuration"]["variant"] == "baseline"
                             and other["configuration"]["preparation"] == config["preparation"]
                             and other["configuration"]["parser_workers"] == config["parser_workers"]
                             and other["configuration"].get("unique_uv_validation", True)
                             == config.get("unique_uv_validation", True)), None)
            if baseline is not None and baseline["probe"]["cpu_load_ms"] > 0:
                ratios.append(row["probe"]["cpu_load_ms"] / baseline["probe"]["cpu_load_ms"])
            if baseline is not None:
                mapping_ms = stage_wall_ms(row["probe"], "mesh_allocation", "corner_mapping")
                baseline_mapping_ms = stage_wall_ms(baseline["probe"], "mesh_allocation", "corner_mapping")
                if baseline_mapping_ms > 0:
                    mapping_ratios.append(mapping_ms / baseline_mapping_ms)
        if ratios:
            paired[key] = {"pairs": len(ratios), "load_ratio_median": statistics.median(ratios),
                           "load_change_percent_median": (statistics.median(ratios) - 1) * 100}
            if mapping_ratios:
                paired[key].update(mapping_pairs=len(mapping_ratios),
                                   mapping_ratio_median=statistics.median(mapping_ratios),
                                   mapping_change_percent_median=(statistics.median(mapping_ratios) - 1) * 100)
    return {"configurations": summary, "paired_with_baseline": paired}


def save_checkpoint(path, data):
    """Replace the summary only after the new file is complete."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(data, stream, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def measure(executable, model, configuration, timeout, snapshot):
    command = [str(executable), str(model), str(configuration["parser_workers"]),
               str(configuration["gather_workers"]), configuration["preparation"]]
    environment = dict(os.environ, WOBY_PROFILE_RESERVE_PERCENT="0")
    if "unique_uv_validation" in configuration:
        environment["WOBY_MAPPING_UNIQUE_UVS"] = "1" if configuration["unique_uv_validation"] else "0"
    snapshot.with_suffix(".command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    snapshot.with_suffix(".environment.json").write_text(json.dumps({
        "WOBY_PROFILE_RESERVE_PERCENT": environment["WOBY_PROFILE_RESERVE_PERCENT"],
        "WOBY_MAPPING_UNIQUE_UVS": environment.get("WOBY_MAPPING_UNIQUE_UVS", "1")}, indent=2), encoding="utf-8")
    start = time.perf_counter()
    try:
        process = subprocess.run(command, capture_output=True, timeout=timeout, env=environment)
        stdout, stderr, returncode = process.stdout, process.stderr, process.returncode
    except subprocess.TimeoutExpired as error:
        snapshot.write_bytes(error.stdout or b"")
        snapshot.with_suffix(".stderr.txt").write_bytes(error.stderr or b"")
        raise
    elapsed = (time.perf_counter() - start) * 1000
    snapshot.write_bytes(stdout)
    snapshot.with_suffix(".stderr.txt").write_bytes(stderr)
    if returncode:
        raise RuntimeError(f"Probe exited with {returncode}; see {snapshot.with_suffix('.stderr.txt')}")
    try:
        probe = json.loads(stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"The probe result is not JSON: {snapshot}") from error
    validate_probe(probe, configuration)
    return {"configuration": configuration, "process_wall_ms": elapsed,
            "stdout_snapshot": str(snapshot), "probe": probe}


def execute_sweep(model, executables, configurations, rounds, timeout, output_path):
    snapshots = output_path.with_suffix(".runs")
    if output_path.exists() or snapshots.exists():
        raise FileExistsError("Use a new output name. Existing measurements are preserved.")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    snapshots.mkdir()
    try:
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                           text=True, stderr=subprocess.DEVNULL).strip()
        dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).splitlines()
    except (OSError, subprocess.CalledProcessError):
        revision, dirty = None, []
    output = {"method": "Fresh Release processes; sequential runs; file warmed before each process; rotating configuration order; reserve percentage fixed at 0",
              "reserve_percent": 0,
              "status": "running", "model": file_identity(model),
              "executables": {variant: file_identity(path) for variant, path in executables.items()},
              "sources": source_identities(), "revision": revision, "worktree_status": dirty,
              "os": platform.platform(), "processor": platform.processor(), "logical_cpus": os.cpu_count(),
              "rounds_requested": rounds, "configurations": configurations,
              "records": [], "summary": {}}
    save_checkpoint(output_path, output)
    references = {}
    try:
        for round_index in range(rounds):
            for configuration in rotated_order(configurations, round_index):
                key = configuration_key(configuration)
                print(f"round={round_index + 1} configuration={key}", flush=True)
                warm_file(model)
                snapshot = snapshots / f"{len(output['records']) + 1:04d}-{key}.stdout.json"
                record = measure(executables[configuration["variant"]], model, configuration, timeout, snapshot)
                record["round"] = round_index + 1
                output["records"].append(record)
                output["summary"] = summarize(output["records"])
                save_checkpoint(output_path, output)
                geometry = canonical_geometry(record["probe"])
                reference_key = configuration["preparation"]
                reference = references.setdefault(reference_key, geometry)
                if geometry != reference:
                    raise ValueError(f"Ordered geometry differs for {key}; measurements are saved at {output_path}")
                mapping = next((event for event in record["probe"]["events"] if event["name"] == "corner_mapping"), None)
                print(json.dumps({"round": record["round"], "configuration": key,
                                  "cpu_load_ms": record["probe"]["cpu_load_ms"],
                                  "mapping_pipeline_ms": stage_wall_ms(record["probe"], "mesh_allocation", "corner_mapping"),
                                  "corner_mapping_ms": mapping["wall_ms"] if mapping else None}), flush=True)
        output["status"] = "complete"
    except BaseException as error:
        output["status"] = "failed"
        output["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        save_checkpoint(output_path, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--bin", type=Path, default=ROOT / "build/vs2026-vcpkg/bin/Release")
    parser.add_argument("--variants", default=DEFAULT_VARIANTS)
    parser.add_argument("--gather-workers", type=parse_workers, default=parse_workers("1,2,8"))
    parser.add_argument("--parser-workers", type=int, default=16)
    parser.add_argument("--preparation", default="deferred")
    parser.add_argument("--unique-uv-validation", default="on", help="Use on, off, or on,off.")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=1200)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (args.rounds < 1 or not math.isfinite(args.timeout) or args.timeout <= 0
            or args.parser_workers < 1 or args.parser_workers > 64):
        parser.error("Use positive rounds and timeout, and 1 to 64 parser workers.")
    variants = args.variants.split(",")
    try:
        uv_modes = args.unique_uv_validation.split(",")
        if any(mode not in {"on", "off"} for mode in uv_modes):
            raise ValueError("Unique UV validation must be on or off.")
        configurations = configurations_for(variants, args.parser_workers, args.gather_workers,
                                              args.preparation.split(","), [mode == "on" for mode in uv_modes])
        suffix = ".exe" if os.name == "nt" else ""
        executables = {variant: (args.bin / f"woby_corner_mapping_{variant}{suffix}").resolve(strict=True)
                       for variant in variants}
        execute_sweep(args.model.resolve(strict=True), executables, configurations, args.rounds,
                      args.timeout, args.output.resolve())
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
