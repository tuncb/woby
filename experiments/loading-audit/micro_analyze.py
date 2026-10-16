# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Reduce parser-only hardware captures, keeping sample support and scope."""
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import statistics
import sys

import capstone
import duckdb
import pefile


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def ratio(numerator, denominator, scale=1):
    return numerator / denominator * scale if denominator else None


def metrics(counts):
    events = {name: row["estimated_events"] for name, row in counts.items()}
    retired, cycles = events.get("0c0:00", 0), events.get("076:00", 0)
    result = {"ipc": ratio(retired, cycles)}
    for key, name in (("0c2:00", "branches_per_kinst"), ("0c3:00", "branch_misses_per_kinst"),
        ("1c7:00", "direction_misses_per_kinst"), ("0ca:00", "indirect_misses_per_kinst"),
        ("043:01", "demand_local_l2_per_kinst"), ("043:02", "demand_l3_peer_l2_per_kinst"),
        ("043:08", "demand_dram_io_per_kinst"), ("029:07", "ls_dispatch_per_kinst"),
        ("045:0f", "dtlb_l2_hits_per_kinst"), ("045:f0", "dtlb_l2_misses_per_kinst"),
        ("047:01", "split_cacheline_loads_per_kinst"), ("047:02", "split_page_loads_per_kinst"),
        ("082:00", "icache_l2_refills_per_kinst"), ("083:00", "icache_system_refills_per_kinst"),
        ("085:07", "itlb_walks_per_kinst"), ("070:ff", "l2_prefetch_hits_per_kinst"),
        ("072:ff", "l2_l3_prefetch_misses_per_kinst")):
        if key in events:
            result[name] = ratio(events[key], retired, 1000)
    if "0c3:00" in events and "0c2:00" in events:
        result["branch_miss_percent"] = ratio(events["0c3:00"], events["0c2:00"], 100)
    if "043:01" in events:
        total = sum(events.get(key, 0) for key in ("043:01", "043:02", "043:08"))
        result["demand_source_percent"] = {key: ratio(events.get(key, 0), total, 100) for key in ("043:01", "043:02", "043:08")}
        result["demand_fills_per_1000_ls_dispatch"] = ratio(total, events.get("029:07", 0), 1000)
    if "064:08" in events and "064:f0" in events:
        result["selected_l2_data_miss_percent"] = ratio(events["064:08"], events["064:08"] + events["064:f0"], 100)
    if "045:f0" in events and "045:0f" in events:
        total = events["045:f0"] + events["045:0f"]
        result["dtlb_refills_per_kinst"] = ratio(total, retired, 1000)
        result["dtlb_refill_l2_miss_percent"] = ratio(events["045:f0"], total, 100)
    for code, name in (("0a9:00", "op_queue_empty_cycle_percent"), ("0ae:02", "load_queue_stall_cycle_percent"),
        ("0ae:04", "store_queue_stall_cycle_percent"), ("0af:0f", "integer_scheduler_stall_cycle_percent"),
        ("0ae:01", "integer_register_stall_cycle_percent")):
        if code in events:
            result[name] = ratio(events[code], cycles, 100)
    return result


def weighted_counts(definitions, values):
    return {f'{event["hwId"]:03x}:{event["mask"]:02x}': {"samples": value or 0,
        "estimated_events": (value or 0) * event["interval"], "interval": event["interval"]}
        for event, value in zip(definitions, values)}


def counter_query(database, expressions, pid, begin, end, threads=None, grouping=None):
    where = "s.processId=? and s.windowId>=? and s.windowId<?"
    arguments = [pid, begin, end]
    if threads is not None:
        if not threads:
            raise ValueError("Empty thread scope")
        where += " and s.threadId in (" + ",".join("?" for _ in threads) + ")"
        arguments += threads
    if grouping == "function":
        return database.execute(f'''select m.path,coalesce(f.name,'[unresolved function]'),{expressions}
            from UnifiedSampleSeries s join Module m on s.moduleId=m.id left join Function f on s.functionId=f.id
            where {where} group by m.path,f.name''', arguments).fetchall()
    if grouping == "core":
        return database.execute(f'''select s.coreId,{expressions} from UnifiedSampleSeries s
            where {where} group by s.coreId''', arguments).fetchall()
    return database.execute(f"select {expressions} from UnifiedSampleSeries s where {where}", arguments).fetchone()


def analyze_capture(entry):
    session = Path(entry["session"])
    record = read(Path(entry["probe"])); metadata = read(session / "session.uprof")
    binary = session / "matching-symbols" / Path(metadata["target"]["program"]).name if "target" in metadata and "program" in metadata["target"] else None
    if binary is None or not binary.exists():
        binary = next((session / "matching-symbols").glob("*.exe"))
    definitions = metadata["collection"]["events"]["pmcEvents"]["pmcEventList"]
    if len(definitions) > 6:
        raise ValueError("Counter set exceeds the six core PMCs")
    offset = (record["clock_origin"]["steady_epoch_nanoseconds"] - metadata["collection"]["schedule"]["start"]) / 1e6
    if not -1000 < offset < 3000:
        raise ValueError("Capture clock mismatch")
    database = duckdb.connect(str(session / "cpu.db"), read_only=True)
    pid = record["clock_origin"]["pid"]
    worker_ids = [row["thread_id"] for row in record["parser_workers"]]
    expressions = ",".join(f'sum(s."event#0x{event["encodedEventId"]:x}")' for event in definitions)
    phases = {row["name"]: row for row in record["events"]}
    specs = [("parser_total", "parser_total", None), ("read_decode_workers", "parser_scan_collect", worker_ids),
             ("read_decode_all", "parser_scan_collect", None), ("attribute_allocation", "parser_attribute_allocation", None),
             ("merge_all", "parser_merge", None), ("merge_execute", "parser_merge_execute", None)]
    scopes = []
    images = {}
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for name, phase_name, threads in specs:
        phase = phases[phase_name]; begin, end = offset + phase["start_ms"], offset + phase["end_ms"]
        counts = weighted_counts(definitions, counter_query(database, expressions, pid, begin, end, threads))
        shifted = weighted_counts(definitions, counter_query(database, expressions, pid, begin + 16, end + 16, threads))
        functions = []
        for module, function, *values in counter_query(database, expressions, pid, begin, end, threads, "function"):
            item = weighted_counts(definitions, values)
            functions.append({"module": module, "function": function, "counts": item, "metrics": metrics(item)})
        functions.sort(key=lambda row: -row["counts"].get("076:00", {}).get("estimated_events", 0))
        cores = [{"core_id": core, "counts": weighted_counts(definitions, values)}
            for core, *values in counter_query(database, expressions, pid, begin, end, threads, "core")]
        observed_cores = {row["core_id"] for row in cores if row["counts"].get("076:00", {}).get("samples", 0)}
        if not observed_cores.issubset(entry["affinity"]):
            raise ValueError("Profiled application left its requested affinity")
        hot_sites = []
        if name == "read_decode_workers":
            selected_events = [d for d in definitions if (d["hwId"], d["mask"]) in ((0xC3, 0), (0x1C7, 0), (0x43, 8), (0x45, 0xF0))]
            placeholders = ",".join("?" for _ in worker_ids)
            for event in selected_events:
                column = f'event#0x{event["encodedEventId"]:x}'
                locations = database.execute(f'''select m.path,coalesce(f.name,'[unresolved function]'),s.moduleRVA,sum(s."{column}") n
                    from UnifiedSampleSeries s join Module m on s.moduleId=m.id left join Function f on s.functionId=f.id
                    where s.processId=? and s.windowId>=? and s.windowId<? and s.threadId in ({placeholders})
                    group by m.path,f.name,s.moduleRVA having n>0 order by n desc limit 20''', [pid, begin, end, *worker_ids]).fetchall()
                for module, function, rva, samples in locations:
                    path = binary if Path(module).suffix.lower() == ".exe" else Path(module)
                    if str(path) not in images:
                        try:
                            images[str(path)] = pefile.PE(str(path), fast_load=True).get_memory_mapped_image()
                        except (OSError, pefile.PEFormatError):
                            images[str(path)] = None
                    instruction = next(decoder.disasm(images[str(path)][rva:rva + 16], rva, 1), None) if images[str(path)] else None
                    hot_sites.append({"event": f'{event["hwId"]:03x}:{event["mask"]:02x}', "module": module,
                        "function": function, "rva": hex(rva), "samples": samples,
                        "instruction": instruction.mnemonic + " " + instruction.op_str if instruction else ""})
        scopes.append({"name": name, "phase": phase_name, "wall_ms": phase["wall_ms"], "cpu_ms": phase["cpu_ms"],
            "user_ms": phase["user_ms"], "kernel_ms": phase["kernel_ms"], "counts": counts, "metrics": metrics(counts),
            "functions": functions, "cores": cores, "hot_sites": hot_sites,
            "alignment_plus_16ms_percent": {key: ratio(shifted[key]["estimated_events"] - value["estimated_events"], value["estimated_events"], 100)
                                           for key, value in counts.items()}})
    database.close()
    return dict(entry, clock_alignment_ms=offset, definitions=definitions, scopes=scopes,
        executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), parser_workers=record["parser_workers"])


def summarize_captures(captures):
    groups = defaultdict(list)
    for capture in captures:
        for scope in capture["scopes"]:
            placement = capture.get("placement", "physical" if capture["workers"] == 8 else "all")
            groups[(capture["kind"], capture["workers"], placement, scope["name"])].append(scope)
    result = []
    for (kind, workers, placement, name), scopes in groups.items():
        counts = {}
        functions = {}
        for scope in scopes:
            for code, value in scope["counts"].items():
                item = counts.setdefault(code, dict(samples=0, estimated_events=0, interval=value["interval"]))
                item["samples"] += value["samples"]; item["estimated_events"] += value["estimated_events"]
            for function in scope["functions"]:
                key = (function["module"], function["function"])
                target = functions.setdefault(key, {"module": key[0], "function": key[1], "counts": {}})
                for code, value in function["counts"].items():
                    item = target["counts"].setdefault(code, dict(samples=0, estimated_events=0, interval=value["interval"]))
                    item["samples"] += value["samples"]; item["estimated_events"] += value["estimated_events"]
        for function in functions.values():
            function["metrics"] = metrics(function["counts"])
        scalar_keys = {key for scope in scopes for key, value in scope["metrics"].items() if isinstance(value, (int, float))}
        per_run = {key: {"median": statistics.median(values), "min": min(values), "max": max(values), "values": values}
                   for key in scalar_keys if (values := [scope["metrics"][key] for scope in scopes if isinstance(scope["metrics"].get(key), (int, float))])}
        result.append({"kind": kind, "workers": workers, "placement": placement, "scope": name, "captures": len(scopes), "counts": counts,
            "metrics": metrics(counts), "per_capture_metrics": per_run,
            "functions": sorted(functions.values(), key=lambda row: -row["counts"].get("076:00", {}).get("estimated_events", 0))})
    return result


def timing_summary(records):
    result = []
    for name in sorted({row["configuration"] for row in records}):
        selected = [row for row in records if row["configuration"] == name]
        phases = [next(p for p in row["events"] if p["name"] == "parser_total") for row in selected]
        values = [p["wall_ms"] for p in phases]
        result.append({"name": name, "affinity": selected[0]["affinity"], "runs": len(selected),
            "wall_ms": {"median": statistics.median(values), "min": min(values), "max": max(values), "values": values},
            "cpu_ms": statistics.median(p["cpu_ms"] for p in phases),
            "scan_ms": statistics.median(next(p["wall_ms"] for p in row["events"] if p["name"] == "parser_scan_collect") for row in selected),
            "merge_ms": statistics.median(next(p["wall_ms"] for p in row["events"] if p["name"] == "parser_merge") for row in selected)})
    return result


def main(capture):
    root = capture / "parser-microarchitecture"
    entries = read(root / "counter-manifest.json")["captures"]
    captures = [analyze_capture(entry) for entry in entries]
    provenance = read(root / "provenance.json")
    if any(row["executable_sha256"] != provenance["executable_sha256"] for row in captures):
        raise ValueError("Counter-capture binary hash mismatch")
    allocation = read(root / "allocation-audit.json")
    expected = read(capture / "parser/results.json")["records"][0]["fingerprint"]
    if allocation["fingerprint"] != expected:
        raise ValueError("Allocation-audit parser output differs")
    growth = allocation["allocation_audit"]["buffers"]
    copies = sum(row["copy_payload_bytes"] for row in growth)
    positions, faces = allocation["positions"], allocation["faces"]
    # This model has no normal records or non-triangle faces; exact census and
    # the validated importer corner count establish these model-specific terms.
    texcoords = read(capture / "census.json")["records"]["vt"]
    chunk_bytes = positions * 24 + texcoords * 16 + faces * (3 * 13 + 1)
    output_bytes = positions * 24 + texcoords * 16 + faces * (3 * 12 + 9)
    metadata_scan = read(root / "metadata-scan.json")
    constant_fields = {"missing_normal_ids": faces * 3 * 4,
        "uniform_material_ids": faces * 4 if metadata_scan["uniform_material_id_proven"] else 0,
        "zero_smoothing_ids": faces * 4 if metadata_scan["uniform_zero_smoothing_proven"] else 0}
    audit_binary = root / "allocation-matching-bin/woby_parser_allocation_audit.exe"
    result = {"provenance": provenance, "topology": read(root / "topology.json"), "captures": captures,
        "groups": summarize_captures(captures), "affinity": timing_summary(read(root / "affinity-timings.json")["records"]),
        "allocation": allocation, "allocation_provenance": {"executable_sha256": hashlib.sha256(audit_binary.read_bytes()).hexdigest(),
            "pdb_sha256": hashlib.sha256(audit_binary.with_suffix(".pdb").read_bytes()).hexdigest()},
        "metadata_scan": metadata_scan, "constant_parser_fields_bytes": constant_fields,
        "traffic": {"input_bytes": provenance["model_bytes"], "temporary_logical_bytes": chunk_bytes,
            "output_logical_bytes": output_bytes, "buffer_growth_copy_payload_bytes": copies,
            "buffer_growth_minimum_read_write_bytes": 2 * copies,
            "cumulative_buffer_allocated_bytes": sum(row["allocated_bytes"] for row in growth),
            "minimum_logical_read_write_bytes": provenance["model_bytes"] + 2 * chunk_bytes + output_bytes + 2 * copies},
        "limits": ["User-mode core events only; Windows file-cache and fault handling in kernel mode are excluded.",
            "Event-based sampling has skid and attribution bias. Ratios are estimates, especially within individual functions.",
            "No observed samples does not prove zero events. The minimum permitted core-event interval is 25000.",
            "Dispatch stalls overlap; op-queue-empty cycles are not a complete frontend-bound measurement.",
            "Demand-fill sources and selected L2 request outcomes are not universal cache hit rates or physical DRAM traffic.",
            "IBS counters are unavailable in this Windows session; precise load latency, dependency chains, and false-sharing attribution remain unmeasured."]}
    (root / "analysis.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"captures": len(captures), "affinity": result["affinity"], "traffic": result["traffic"]}))


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(strict=True))
