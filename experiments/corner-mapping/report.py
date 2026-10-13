# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Write a static report. Keep each runner input as a separate measurement batch."""
import argparse
import csv
import hashlib
import html
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import statistics
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
BASE_REVISION = "b83bd71"
VARIANTS = ("baseline", "pointer", "split", "dense", "compact", "release",
            "compact_release", "fast", "fast_release")
GENERATED_FILES = ("report.html", "summary.csv", "passes.csv", "buffers.csv", "measurements.zip")
VARIANT_NOTES = {
    "baseline": "Resolve each corner and gather each new vertex in the same loop.",
    "pointer": "Keep the baseline loop. Capture the UV data pointer.",
    "split": "Resolve first-use IDs. Then gather attributes from ordered vertex keys.",
    "dense": "Keep the primary key and render ID in one entry. Use separate attribute gathering.",
    "compact": "Store primary keys in source-position slots. Store full keys only for seams. Gather primary vertices and seams in separate passes.",
    "release": "Use separate gathering. Release resolved triangle input and unused lookup arrays before gathering when parsed shapes have no explicit line or point records.",
    "compact_release": "Use compact primary entries. Release resolved triangle input before gathering when parsed shapes have no explicit line or point records.",
    "fast": "Use compact entries. Allocate index arrays once and write through their data pointers. Inline primary resolution. Gather attributes in separate passes.",
    "fast_release": "Use the fast resolver and compact gathering. Release resolved triangle input before gathering when parsed shapes have no explicit line or point records.",
}


def esc(value):
    return html.escape(str(value), quote=True)


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def median(values):
    values = [value for value in values if number(value)]
    return statistics.median(values) if values else None


def numeric_leaves(value, prefix=""):
    if isinstance(value, dict):
        for key, child in value.items():
            yield from numeric_leaves(child, f"{prefix}.{key}" if prefix else key)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from numeric_leaves(child, f"{prefix}.{index}")
    elif number(value):
        yield prefix, value


def configuration_key(config):
    return (f"{config['variant']}-p{config['parser_workers']}-g{config['gather_workers']}"
            f"-{config['preparation']}-u{int(config.get('unique_uv_validation', True))}")


def event_values(probe, name, field="wall_ms"):
    return [event[field] for event in probe["events"]
            if event["name"] == name and number(event.get(field))]


def pass_total(probe, *names):
    values = [event["wall_ms"] for event in probe["events"]
              if event["name"] in names and number(event.get("wall_ms"))]
    return sum(values) if values else None


def mapping_total(probe):
    # The child mapping_* phases are already inside corner_mapping.
    return pass_total(probe, "mesh_allocation", "corner_mapping")


def memory_value(probe, event_name, field, maximum=False):
    values = event_values(probe, event_name, field)
    return max(values) if maximum and values else values[-1] if values else None


def retained_capacity(probe):
    memory = probe.get("memory", {})
    values = [value for field, value in memory.items() if field.endswith("_capacity_bytes")]
    return sum(values) if values and all(number(value) for value in values) else None


def sort_configuration(config):
    variant = config["variant"]
    order = VARIANTS.index(variant) if variant in VARIANTS else len(VARIANTS)
    return (config["preparation"], config["parser_workers"], order,
            config["gather_workers"], not config.get("unique_uv_validation", True))


def paired_reductions(records, config):
    """Pair within one input, one round, and the same parser/preparation/UV mode."""
    reductions = {"mapping": [], "load": []}
    key = configuration_key(config)
    for row in records:
        if configuration_key(row["configuration"]) != key:
            continue
        matches = [other for other in records
                   if other["round"] == row["round"]
                   and other["configuration"]["variant"] == "baseline"
                   and other["configuration"]["parser_workers"] == config["parser_workers"]
                   and other["configuration"]["preparation"] == config["preparation"]
                   and other["configuration"].get("unique_uv_validation", True)
                   == config.get("unique_uv_validation", True)]
        if len(matches) > 1:
            raise ValueError("A round has more than one matching baseline; pairing is not defined.")
        if not matches:
            continue
        baseline = matches[0]["probe"]
        for name, before, after in (("mapping", mapping_total(baseline), mapping_total(row["probe"])),
                                    ("load", baseline["cpu_load_ms"], row["probe"]["cpu_load_ms"])):
            if number(before) and before > 0 and number(after):
                reductions[name].append((1 - after / before) * 100)
    return reductions


def summarize_batch(batch):
    groups = {}
    for row in batch["data"]["records"]:
        groups.setdefault(configuration_key(row["configuration"]), []).append(row)
    result = []
    for key, rows in sorted(groups.items(), key=lambda item: sort_configuration(item[1][0]["configuration"])):
        config = rows[0]["configuration"]
        pairs = paired_reductions(batch["data"]["records"], config)
        mapping_times = [mapping_total(row["probe"]) for row in rows]
        load_times = [row["probe"]["cpu_load_ms"] for row in rows]
        result.append({"batch": batch["label"], "batch_kind": batch["kind"], "configuration": key,
                       **config, "runs": len(rows),
                       "mapping_median_ms": median(mapping_times),
                       "mapping_min_ms": min((value for value in mapping_times if number(value)), default=None),
                       "mapping_max_ms": max((value for value in mapping_times if number(value)), default=None),
                       "cpu_load_median_ms": median(load_times), "cpu_load_min_ms": min(load_times),
                       "cpu_load_max_ms": max(load_times),
                       "mapping_pairs": len(pairs["mapping"]),
                       "mapping_reduction_percent": median(pairs["mapping"]),
                       "load_pairs": len(pairs["load"]), "load_reduction_percent": median(pairs["load"]),
                       "mapping_end_rss_bytes": median(memory_value(row["probe"], "corner_mapping", "working_set_end") for row in rows),
                       "load_peak_rss_bytes": median(memory_value(row["probe"], "cpu_import", "process_peak_working_set", True) for row in rows),
                       "load_peak_commit_bytes": median(memory_value(row["probe"], "cpu_import", "process_peak_commit", True) for row in rows),
                       "retained_capacity_bytes": median(retained_capacity(row["probe"]) for row in rows),
                       "retained_vertex_bytes": median(row["probe"].get("memory", {}).get("vertex_capacity_bytes") for row in rows),
                       "rows": rows})
    return result


def read_batch(path, index):
    path = path.resolve(strict=True)
    data = json.loads(path.read_bytes())
    if not isinstance(data, dict) or not isinstance(data.get("records"), list) or not data["records"]:
        raise ValueError(f"No runner records in {path}")
    seen = set()
    for row in data["records"]:
        if not isinstance(row.get("probe"), dict) or not isinstance(row["probe"].get("events"), list):
            raise ValueError(f"Missing probe events in {path}")
        key = (row.get("round"), configuration_key(row["configuration"]))
        if key in seen:
            raise ValueError(f"Repeated round and configuration in {path}: {key}")
        seen.add(key)
        if not number(row["probe"].get("cpu_load_ms")):
            raise ValueError(f"Invalid CPU load time in {path}")
    kind = "confirmation" if any(word in path.stem.lower() for word in ("confirm", "final")) else "exploratory"
    return {"path": path, "data": data, "label": f"{index:02d}-{path.stem}", "kind": kind}


def fmt(value, scale=1, digits=3):
    return "—" if value is None else f"{value / scale:,.{digits}f}"


def exact(value):
    if value is None:
        return "—"
    return f"{value:,.0f}" if int(value) == value else f"{value:,.3f}"


def table(headers, rows):
    head = "".join(f"<th>{esc(item)}</th>" for item in headers)
    body = "".join("<tr>" + "".join(f"<td>{esc(item)}</td>" for item in row) + "</tr>" for row in rows)
    return f'<div class="scroll"><table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table></div>'


def pass_rows(batches):
    result = []
    for batch in batches:
        for row in batch["data"]["records"]:
            for event_index, event in enumerate(row["probe"]["events"]):
                result.append({"batch": batch["label"], "batch_kind": batch["kind"],
                               "round": row["round"], "configuration": configuration_key(row["configuration"]),
                               "event_index": event_index, "phase": event["name"], "parent": event.get("parent", ""),
                               **{field: event.get(field) for field in
                                  ("wall_ms", "cpu_ms", "working_set_begin", "working_set_end", "commit_begin",
                                   "commit_end", "process_peak_working_set", "process_peak_commit")},
                               "data": json.dumps(event.get("data", {}), sort_keys=True, separators=(",", ":"))})
    return result


def buffer_rows(batches):
    result = []
    for batch in batches:
        for row in batch["data"]["records"]:
            common = {"batch": batch["label"], "batch_kind": batch["kind"], "round": row["round"],
                      "configuration": configuration_key(row["configuration"])}
            for event in row["probe"]["events"]:
                for field, value in numeric_leaves(event.get("data", {})):
                    result.append({**common, "phase": event["name"], "field": field,
                                   "unit": "bytes" if "bytes" in field else "count", "value": value})
            for field, value in numeric_leaves(row["probe"].get("memory", {})):
                result.append({**common, "phase": "retained_memory", "field": field,
                               "unit": "bytes" if "bytes" in field else "count", "value": value})
    return result


def csv_text(rows, fields):
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
    writer.writeheader()
    writer.writerows(rows)
    return stream.getvalue()


def atomic_write(path, content):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content.encode("utf-8") if isinstance(content, str) else content)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def matching_source(relative, identity):
    """Use recorded or current files only when bytes match the recorded source hash."""
    safe = PurePosixPath(relative.replace("\\", "/"))
    if safe.is_absolute() or ".." in safe.parts or ":" in str(safe):
        return None, "invalid source path"
    recorded_path = identity.get("path")
    candidates = [Path(recorded_path)] if recorded_path else []
    candidates.append(ROOT.joinpath(*safe.parts))
    expected = identity.get("sha256")
    for candidate in dict.fromkeys(candidates):
        if candidate.is_file():
            content = candidate.read_bytes()
            if hashlib.sha256(content).hexdigest() == expected:
                return content, "matched"
    return None, "source is absent or its hash has changed"


def snapshot_path(batch, row):
    recorded = Path(row["stdout_snapshot"]) if row.get("stdout_snapshot") else None
    candidates = [recorded] if recorded else []
    if recorded:
        candidates.append(batch["path"].with_suffix(".runs") / recorded.name)
    for candidate in dict.fromkeys(candidates):
        if candidate.is_file():
            content = candidate.read_bytes()
            if json.loads(content) != row["probe"]:
                raise ValueError(f"Raw stdout differs from the runner probe: {candidate}")
            return candidate
    return None


def archive_measurements(batches, output, validation_path):
    manifest = {"source_base": BASE_REVISION, "batches": [], "source_policy":
                "A source file is included only when its bytes match the SHA-256 in that batch. Executables, DLLs, and symbols are excluded."}
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output, prefix="measurements.", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for batch in batches:
                prefix = "batches/" + batch["label"]
                archive.writestr(prefix + "/runner.json", batch["path"].read_bytes())
                entry = {"label": batch["label"], "kind": batch["kind"], "input_path": str(batch["path"]),
                         "revision": batch["data"].get("revision"), "sources": {}, "stdout": []}
                for index, row in enumerate(batch["data"]["records"], 1):
                    snapshot = snapshot_path(batch, row)
                    name = f"{index:04d}-{configuration_key(row['configuration'])}.stdout.json"
                    if snapshot:
                        archive.writestr(prefix + "/runs/" + name, snapshot.read_bytes())
                        for suffix in (".stderr.txt", ".command.json", ".environment.json"):
                            extra = snapshot.with_suffix(suffix)
                            if extra.is_file():
                                archive.writestr(prefix + "/runs/" + name.removesuffix(".json") + suffix, extra.read_bytes())
                        status = "original raw stdout"
                    else:
                        archive.writestr(prefix + "/runs/" + name,
                                         json.dumps(row["probe"], indent=2, allow_nan=False) + "\n")
                        status = "probe JSON reconstructed from runner; raw stdout is absent"
                    entry["stdout"].append({"round": row["round"], "configuration": configuration_key(row["configuration"]),
                                            "archive_path": prefix + "/runs/" + name, "status": status})
                for relative, identity in sorted(batch["data"].get("sources", {}).items()):
                    content, status = matching_source(relative, identity)
                    entry["sources"][relative] = {"sha256": identity.get("sha256"), "status": status}
                    if content is not None:
                        archive.writestr(prefix + "/sources/" + relative.replace("\\", "/"), content)
                manifest["batches"].append(entry)
            archive.writestr("tools/report.py", Path(__file__).read_bytes())
            if validation_path:
                archive.writestr("validation.json", validation_path.read_bytes())
            for name in ("summary.csv", "passes.csv", "buffers.csv"):
                archive.writestr(name, (output / name).read_bytes())
            archive.writestr("manifest.json", json.dumps(manifest, indent=2, allow_nan=False) + "\n")
        os.replace(temporary, output / "measurements.zip")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return manifest


def reproduction(batch):
    data = batch["data"]
    configs = data.get("configurations") or [row["configuration"] for row in data["records"]]
    variants = list(dict.fromkeys(config["variant"] for config in configs))
    targets = " ".join("woby_corner_mapping_" + variant for variant in variants)
    commands = ["cmake --preset vs2026-vcpkg -DBUILD_TESTING=ON -DWOBY_BUILD_CORNER_MAPPING=ON -DWOBY_BUILD_OBJ_PROFILE=OFF",
                "cmake --build --preset vs2026-vcpkg --config Release --target " + targets]
    parser_counts = sorted({config["parser_workers"] for config in configs})
    rounds = data.get("rounds_requested", max(row["round"] for row in data["records"]))
    model = data.get("model", {}).get("path", "MODEL.obj")
    binaries = [identity.get("path") for identity in data.get("executables", {}).values() if identity.get("path")]
    parents = {str(Path(path).parent) for path in binaries}
    bin_path = next(iter(parents)) if len(parents) == 1 else "build/vs2026-vcpkg/bin/Release"
    for index, parser_workers in enumerate(parser_counts, 1):
        selected = [config for config in configs if config["parser_workers"] == parser_workers]
        gather = sorted({config["gather_workers"] for config in selected if config["variant"] not in {"baseline", "pointer"}}) or [1]
        preparation = list(dict.fromkeys(config["preparation"] for config in selected))
        uv = list(dict.fromkeys("on" if config.get("unique_uv_validation", True) else "off" for config in selected))
        selected_variants = list(dict.fromkeys(config["variant"] for config in selected))
        command = ["uv", "run", "experiments/corner-mapping/run.py", model, "--bin", bin_path,
                   "--variants", ",".join(selected_variants), "--parser-workers", str(parser_workers),
                   "--gather-workers", ",".join(map(str, gather)), "--preparation", ",".join(preparation),
                   "--unique-uv-validation", ",".join(uv), "--rounds", str(rounds),
                   "--output", f"doc/benchmarks/corner-mapping-reproduction/{batch['label']}-p{parser_workers}.json"]
        commands.append(subprocess.list2cmdline(command))
    return "\n".join(commands)


def geometry_matches(batch):
    groups = {}
    for row in batch["data"]["records"]:
        key = row["configuration"]["preparation"]
        geometry = {field: value for field, value in row["probe"].get("geometry", {}).items()
                    if field != "retained_buffer_capacity_bytes"}
        if not geometry:
            return False
        groups.setdefault(key, []).append(geometry)
    return all(all(value == values[0] for value in values) for values in groups.values())


def batch_html(batch, summaries, all_passes, all_buffers, source_entry):
    data = batch["data"]
    revision = data.get("revision", "unknown")
    model = data.get("model", {})
    sections = [f'<section id="{esc(batch["label"])}"><div class="eyebrow">{esc(batch["kind"])} batch</div>'
                f'<h2>{esc(batch["path"].name)}</h2>',
                f'<p>Status: <strong>{esc(data.get("status", "unknown"))}</strong>. '
                f'Recorded revision: <code>{esc(revision)}</code>. '
                f'Model: <code>{esc(model.get("path", "unknown"))}</code>, '
                f'{esc(exact(model.get("bytes")))} bytes. SHA-256: <code>{esc(model.get("sha256", "unknown"))}</code>.</p>',
                '<p>Each row uses records from this input only. The reduction is the median of same-round reductions '
                'against baseline with the same parser count, preparation mode, and UV mode. '
                'A positive reduction means less time. No pair means that no matching baseline was recorded.</p>']
    sections.append(table(("Configuration", "Runs", "Mapping s (median)", "Mapping s (min–max)", "CPU load s (median)",
                           "Mapping reduction %", "Load reduction %", "Mapping/load pairs"),
                          ((row["configuration"], row["runs"], fmt(row["mapping_median_ms"], 1000),
                            fmt(row["mapping_min_ms"], 1000) + "–" + fmt(row["mapping_max_ms"], 1000),
                            fmt(row["cpu_load_median_ms"], 1000), fmt(row["mapping_reduction_percent"], digits=1),
                            fmt(row["load_reduction_percent"], digits=1), f'{row["mapping_pairs"]}/{row["load_pairs"]}')
                           for row in summaries)))
    sections.append('<h3>Memory at mapping completion and during CPU load</h3><p>RSS is the process working set. '
                    'Mapping completion is one observation. Load peak RSS is the process high-water mark at the end of '
                    '<code>cpu_import</code>. It includes the parser. Early release can reduce mapping RSS without reducing that peak.</p>')
    sections.append(table(("Configuration", "Mapping end RSS MiB", "Load peak RSS MiB", "Load peak commit MiB",
                           "Retained arrays MiB", "Retained vertices MiB"),
                          ((row["configuration"], fmt(row["mapping_end_rss_bytes"], 2**20, 1),
                            fmt(row["load_peak_rss_bytes"], 2**20, 1), fmt(row["load_peak_commit_bytes"], 2**20, 1),
                            fmt(row["retained_capacity_bytes"], 2**20, 1), fmt(row["retained_vertex_bytes"], 2**20, 1))
                           for row in summaries)))
    capacity_fields = sorted({field for row in data["records"] for field in row["probe"].get("memory", {})
                              if field.endswith("_capacity_bytes")})
    sections.append('<p>Retained arrays sum only the reported <code>*_capacity_bytes</code> fields: '
                    + ', '.join(f'<code>{esc(field)}</code>' for field in capacity_fields)
                    + '. Size fields are not added. Earlier batches can report fewer arrays. '
                    'Nodes, strings, thread stacks, allocator overhead, and GPU memory are outside this count. '
                    'It is an array-capacity count, not total process memory.</p><h3>Passes and exact array counters</h3>')
    for summary in summaries:
        key = summary["configuration"]
        passes = [row for row in all_passes if row["batch"] == batch["label"] and row["configuration"] == key]
        grouped = {}
        for row in passes:
            grouped.setdefault((row["phase"], row["parent"]), []).append(row)
        sections.append(f'<details><summary>{esc(key)}: pass times and buffer counters</summary>')
        sections.append(table(("Phase", "Parent", "Observations", "Median wall ms", "Median CPU ms"),
                              ((phase, parent, len(rows), fmt(median(row["wall_ms"] for row in rows)),
                                fmt(median(row["cpu_ms"] for row in rows)))
                               for (phase, parent), rows in grouped.items())))
        counters = {}
        for row in all_buffers:
            if row["batch"] == batch["label"] and row["configuration"] == key:
                counters.setdefault((row["phase"], row["field"], row["unit"]), []).append(row["value"])
        sections.append(table(("Phase", "Counter", "Unit", "Median exact value", "Minimum", "Maximum", "Median MiB"),
                              ((phase, field, unit, exact(median(values)), exact(min(values)), exact(max(values)),
                                fmt(median(values), 2**20, 3) if unit == "bytes" else "—")
                               for (phase, field, unit), values in sorted(counters.items()))))
        sections.append('</details>')
    matched = sum(item["status"] == "matched" for item in source_entry["sources"].values())
    total = len(source_entry["sources"])
    raw_count = sum(item["status"] == "original raw stdout" for item in source_entry["stdout"])
    sections.append(f'<h3>Checks and source record</h3><p>Ordered geometry comparison within this batch: '
                    f'<strong>{"passed" if geometry_matches(batch) else "failed or absent"}</strong>. '
                    f'The comparison includes ordered indices, positions, UVs, normals, source data, bounds, and group records '
                    f'when the probe provides them. The redesign keeps global first-use vertex IDs.</p>'
                    f'<p>The archive has {matched}/{total} source files with matching recorded hashes and '
                    f'{raw_count}/{len(source_entry["stdout"])} original stdout files. '
                    f'<code>manifest.json</code> lists absent or changed source files. The original runner file records all '
                    f'executable and source hashes. Executables, DLLs, and symbols are excluded.</p>'
                    '<details><summary>Reproduction commands</summary><p>Run from the repository root. Use a new output path. '
                    'Use source files with this batch’s recorded hashes. A changed source file can change the result. '
                    'The commands below recreate the recorded variant and worker sweep.</p>'
                    f'<pre>{esc(reproduction(batch))}</pre></details></section>')
    return "\n".join(sections)


def report_html(batches, summaries, passes, buffers, manifest, validation):
    links = " · ".join(f'<a href="#{esc(batch["label"])}">{esc(batch["path"].stem)}</a>' for batch in batches)
    completed = [batch for batch in batches if batch["data"].get("status") == "complete"]
    confirmation = [batch for batch in completed if batch["kind"] == "confirmation"]
    featured_batch = (confirmation or completed)[-1] if completed else None
    result = ""
    if featured_batch:
        candidates = [row for row in summaries if row["batch"] == featured_batch["label"]
                      and row["variant"] == "fast_release" and row["mapping_pairs"]
                      and number(row["mapping_reduction_percent"])]
        if candidates:
            selected = max(candidates, key=lambda row: row["mapping_reduction_percent"])
            reference = next(row for row in summaries if row["batch"] == selected["batch"]
                             and row["variant"] == "baseline"
                             and row["parser_workers"] == selected["parser_workers"]
                             and row["preparation"] == selected["preparation"]
                             and row.get("unique_uv_validation", True) == selected.get("unique_uv_validation", True))
            result = (f'<section><div class="eyebrow">{esc(featured_batch["kind"])} result · '
                      f'{esc(featured_batch["path"].name)}</div><h2>{esc(selected["configuration"])}</h2>'
                      '<p>The selected fast-release configuration has the largest paired mapping reduction among '
                      f'the fast-release configurations in this batch: <strong>{fmt(selected["mapping_reduction_percent"], digits=1)}%</strong> '
                      f'across {selected["mapping_pairs"]} pairs.</p>'
                      + table(("Measurement", "Baseline median", "Variant median"),
                              (("Mapping seconds", fmt(reference["mapping_median_ms"], 1000), fmt(selected["mapping_median_ms"], 1000)),
                               ("CPU load seconds", fmt(reference["cpu_load_median_ms"], 1000), fmt(selected["cpu_load_median_ms"], 1000)),
                               ("Mapping end RSS MiB", fmt(reference["mapping_end_rss_bytes"], 2**20, 1), fmt(selected["mapping_end_rss_bytes"], 2**20, 1)),
                               ("Load peak RSS MiB", fmt(reference["load_peak_rss_bytes"], 2**20, 1), fmt(selected["load_peak_rss_bytes"], 2**20, 1)),
                               ("Retained array capacities MiB", fmt(reference["retained_capacity_bytes"], 2**20, 1), fmt(selected["retained_capacity_bytes"], 2**20, 1))))
                      + '<p>The median time ratio and the median paired reduction are different calculations. '
                      'Lower mapping RSS does not establish a lower CPU load peak.</p></section>')
    body = ['<header><div class="eyebrow">woby · OBJ loader experiment</div><h1>Corner mapping</h1>'
            '<p class="lead">Resolve vertex IDs first. Gather attributes in separate passes. '
            'Measure the time and memory cost of each pass.</p>'
            f'<p>Source base: <code>{BASE_REVISION}</code>. Each input below is a separate batch. '
            'Exploratory and confirmation results are kept separate. Dates, builds, and worker settings are not pooled. '
            'These results come from optional research probes. The production corner-mapping code is unchanged.</p>'
            f'<nav>{links}</nav></header>', result,
            '<section><h2>Method and limits</h2><p>Each measurement uses a fresh Release process. '
            'The input file is read before each process to warm the file cache. Runs are sequential. '
            'Configuration order rotates each round.</p>'
            '<p><strong>Mapping total = mesh_allocation + corner_mapping.</strong> '
            'Resolve, input release, vertex allocation, position mapping, and gather phases are children of '
            '<code>corner_mapping</code>. Their times are shown for inspection. They are not added to the total again.</p>'
            '<p>CPU load is the wall time of <code>cpu_import</code>. Process wall time also includes geometry checks and '
            'process start and exit. This report makes no claim about first-frame time, GPU upload, or full feature readiness.</p>'
            '<p>Byte counters describe array capacities or logical data accesses in the measured code. '
            'They do not measure cache traffic, physical memory bandwidth, or page residency. Initialization bytes count '
            'the array elements initialized by the operation; initializers can set default values as well as zero. '
            'A counter marked <code>upper_bound</code> is an upper bound. Missing counters are not treated as zero.</p>'
            '<p>Primary entries and seam keys are temporary lookup arrays. The split variants retain ordered keys for every '
            'unique vertex. Compact variants use source-position slots for primary keys and full keys only for seam vertices. '
            'They allocate no second corner array. Source-order gathering writes to the original first-use render IDs. '
            'Gather worker counts change gathering; resolution remains serial.</p>'
            '<p>Compact primary entries use 12 bytes per source position. The baseline primary map uses 4 bytes per '
            'source position. The extra 8 bytes per source position can increase resident memory when many source '
            'positions are unused. Ordered keys are removed for primary vertices, but this saving depends on how many '
            'positions the model uses.</p>'
            '<p>For mixed faces and explicit line or point records, the split path allocates the exact face vertex count '
            'before gathering. The vertex array can grow when later primitives add vertices. Early triangle input release '
            'is skipped. A path with no triangle indices keeps the existing direct table and vertex reservation.</p>'
            '<p>Early input release depends on parsed shapes having no explicit line or point records. Freeform patches '
            'can still be appended later. The recorded <code>triangle_only</code> flag describes this parsed-shape check; '
            'it does not require the final model to contain only triangles.</p>',
            table(("Variant", "Change"), ((variant, VARIANT_NOTES[variant]) for variant in VARIANTS)), '</section>']
    for batch, source_entry in zip(batches, manifest["batches"]):
        body.append(batch_html(batch, [row for row in summaries if row["batch"] == batch["label"]],
                               passes, buffers, source_entry))
    body.append('<section><h2>Validation</h2>')
    if validation is None:
        body.append('<p>No separate validation JSON was supplied. The batch geometry comparisons above are available. '
                    'Unit-test and Release-build results are not inferred from benchmark output.</p>')
    else:
        body.append('<p>The validation record supplied with this report is shown below. '
                    'The same JSON is in the measurement archive.</p><pre>'
                    + esc(json.dumps(validation, indent=2, allow_nan=False)) + '</pre>')
    body.append('</section><section><h2>Files</h2><p><a href="summary.csv">Summary CSV</a> · '
                '<a href="passes.csv">Every measured pass</a> · <a href="buffers.csv">Every exact array counter</a> · '
                '<a href="measurements.zip">Raw measurements and matching source files</a></p>'
                '<p>Summary rows remain grouped by batch. Pass and buffer CSV files retain the round, configuration, '
                'phase, and exact measured values. The archive retains runner inputs without changes.</p></section>')
    style = """
:root{color-scheme:light;--ink:#18312e;--muted:#566a66;--line:#d4dfda;--accent:#126756}
*{box-sizing:border-box}body{margin:0;background:#f4f5ef;color:var(--ink);font:16px/1.55 system-ui,sans-serif}
main{max-width:1380px;margin:auto;padding:44px 32px 70px}header{padding:16px 0 36px;border-bottom:2px solid var(--ink)}
h1{font-size:clamp(42px,6vw,76px);line-height:1.05;letter-spacing:-.045em;margin:14px 0 22px}
h2{font-size:28px;line-height:1.2;margin:14px 0 20px}h3{margin:30px 0 12px;font-size:19px}
p{max-width:1050px;margin:12px 0}.lead{font-size:23px;max-width:850px}.eyebrow{text-transform:uppercase;letter-spacing:.14em;font-size:12px;font-weight:750;color:var(--accent)}
section{padding:30px 0;border-bottom:1px solid var(--line)}a{color:var(--accent);text-underline-offset:3px}nav{margin-top:24px;font-size:14px}
.scroll{overflow-x:auto;margin:16px 0}table{border-collapse:collapse;width:100%;font-size:13px;background:#fff}
th{background:#e3ece7;color:var(--ink);text-align:left;font-weight:700}td,th{padding:10px 12px;border-bottom:1px solid var(--line);vertical-align:top}
td:not(:first-child){font-variant-numeric:tabular-nums}tbody tr:hover{background:#eff6f1}code{font-size:.86em;overflow-wrap:anywhere}
pre{white-space:pre-wrap;overflow-wrap:anywhere;padding:20px;background:#e8ede8;font:13px/1.5 ui-monospace,monospace}
details{margin:15px 0;padding:12px 15px;border:1px solid var(--line);background:#f9faf7}summary{cursor:pointer;font-weight:650}
@media(max-width:700px){main{padding:24px 16px}h2{font-size:24px}.lead{font-size:19px}}
"""
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>woby corner mapping measurements</title><style>' + style
            + '</style></head><body><main>' + "\n".join(body) + '</main></body></html>\n')


def generate(inputs, output, validation_path=None):
    output = output.resolve()
    batches = [read_batch(path, index) for index, path in enumerate(inputs, 1)]
    if len({batch["path"] for batch in batches}) != len(batches):
        raise ValueError("An input was listed more than once.")
    validation_path = validation_path.resolve(strict=True) if validation_path else None
    input_paths = {batch["path"] for batch in batches}
    if validation_path:
        input_paths.add(validation_path)
    if any(output / name in input_paths for name in GENERATED_FILES):
        raise ValueError("An output file would replace an input file.")
    output.mkdir(parents=True, exist_ok=True)
    summaries = [row for batch in batches for row in summarize_batch(batch)]
    passes, buffers = pass_rows(batches), buffer_rows(batches)
    summary_fields = [field for field in summaries[0] if field != "rows"]
    atomic_write(output / "summary.csv", csv_text(summaries, summary_fields))
    atomic_write(output / "passes.csv", csv_text(passes, list(passes[0]) if passes else
                 ("batch", "batch_kind", "round", "configuration", "phase", "parent", "wall_ms", "cpu_ms", "data")))
    atomic_write(output / "buffers.csv", csv_text(buffers, ("batch", "batch_kind", "round", "configuration",
                                                          "phase", "field", "unit", "value")))
    manifest = archive_measurements(batches, output, validation_path)
    validation = json.loads(validation_path.read_bytes()) if validation_path else None
    atomic_write(output / "report.html", report_html(batches, summaries, passes, buffers, manifest, validation))
    return {"output": str(output / "report.html"), "batches": len(batches), "configurations": len(summaries),
            "records": sum(len(batch["data"]["records"]) for batch in batches),
            "source_files_matched": sum(sum(item["status"] == "matched" for item in batch["sources"].values())
                                        for batch in manifest["batches"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, action="append", required=True, help="Runner JSON; repeat for separate batches.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--validation", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(generate(args.input, args.output, args.validation), allow_nan=False))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
