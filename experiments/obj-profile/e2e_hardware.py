# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Translate captures, align phase windows, and export native function evidence."""
import argparse
import json
from pathlib import Path
import subprocess

import duckdb
from deep_tools import summarize, UPROF
from production_tools import analyze_production


def sample_thread_ids(native_id, sample_ids, thread_map):
    """Use the sample table's ID domain, which can contain native IDs directly."""
    if native_id in sample_ids:
        return [native_id]
    return [value for value in thread_map.get(native_id, []) if value in sample_ids]


def caller_counts(session, viewer, summary):
    database = duckdb.connect(str(session / "cpu.db"), read_only=True)
    tables = {r[0] for r in database.execute("show tables").fetchall()}
    columns = [r[0] for r in database.execute("describe UnifiedSampleSeries").fetchall()]
    thread_ids = {r[0] for r in database.execute("select distinct threadId from UnifiedSampleSeries").fetchall()}
    thread_map = {}
    schemas = {}
    for table in sorted(tables):
        if "thread" in table.lower():
            fields = [r[0] for r in database.execute('describe "' + table + '"').fetchall()]
            schemas[table] = fields
            native_column = next((c for c in fields if c.lower() in ("tid", "threadid", "osthreadid", "nativethreadid") and c != "id"), None)
            id_column = next((c for c in fields if c.lower() == "id"), None)
            if native_column and id_column:
                for key, value in database.execute(f'select "{id_column}","{native_column}" from "{table}"').fetchall():
                    thread_map.setdefault(value, []).append(key)
    offset = summary["clock_alignment_ms"]
    definitions = summary["event_definitions"]
    phases = [p for p in viewer["trace"] if p["kind"] == "phase" and p["wall_ms"] >= 100]
    for phase, stage in zip(phases, summary["stages"]):
        native_id = phase["thread_id"]
        internal = sample_thread_ids(native_id, thread_ids, thread_map)
        stage["caller_os_thread_id"] = native_id
        if not internal:
            stage["caller_counter_error"] = "No native thread mapping"
            continue
        placeholders = ','.join('?' for _ in internal)
        expressions = ','.join(f'sum("event#0x{d["encodedEventId"]:x}")' for d in definitions)
        values = database.execute(f'select {expressions} from UnifiedSampleSeries where windowId>=? and windowId<? and threadId in ({placeholders})',
                                  [offset + phase["start_ms"], offset + phase["end_ms"], *internal]).fetchone()
        counts = {f'{d["hwId"]:03x}:{d["mask"]:02x}': {"samples": v or 0, "estimated_events": (v or 0)*d["interval"]}
                  for d, v in zip(definitions, values)}
        stage["caller_counts"] = counts
        cycles = counts.get("076:00", {}).get("estimated_events", 0)
        retired = counts.get("0c0:00", {}).get("estimated_events", 0)
        stage["caller_ipc_estimate"] = retired/cycles if cycles else None
    summary["thread_schemas"] = schemas
    summary["native_thread_map"] = thread_map
    worker_rows = []
    for worker in (t for t in viewer['trace'] if t['kind']=='worker'):
        ids = sample_thread_ids(worker['thread_id'], thread_ids, thread_map)
        row = dict(worker)
        if ids:
            placeholders = ','.join('?' for _ in ids)
            values = database.execute(f'select {expressions} from UnifiedSampleSeries where windowId>=? and windowId<? and threadId in ({placeholders})',
                                      [offset+worker['start_ms'], offset+worker['end_ms'], *ids]).fetchone()
            row['counts'] = {f'{d["hwId"]:03x}:{d["mask"]:02x}': {'samples': v or 0, 'estimated_events': (v or 0)*d['interval']}
                             for d,v in zip(definitions,values)}
            cycles=row['counts'].get('076:00',{}).get('estimated_events',0)
            retired=row['counts'].get('0c0:00',{}).get('estimated_events',0)
            row['ipc_estimate']=retired/cycles if cycles else None
        worker_rows.append(row)
    summary['parser_worker_counters']=worker_rows
    database.close()
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    binary = root / "matching-bin"
    for case in ("branch", "cache", "fp", "production-branch"):
        output = root / "deep" / case
        session = output / "cpu"
        if not (session / "session.uprof").exists():
            print("Missing capture " + case, flush=True); continue
        with (output / "translation.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(UPROF), "report", "-i", str(session), "--cutoff", "0", "--symbol-path", str(binary)],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=900)
        if result.returncode or not (session / "cpu.db").exists():
            raise RuntimeError("Translation failed: " + case)
        viewer = json.loads((output / "result.json").read_text())
        if case == "production-branch":
            result = analyze_production(binary / "woby.exe", binary / "production-woby.map", session, output / "result.json")
            (output / "production-analysis.json").write_text(json.dumps(result, indent=2))
            print("Production native function samples: " + str(len(result["functions"])), flush=True)
        else:
            metadata = next(t for t in viewer["trace"] if t["kind"] == "metadata")
            probe = {"mode": "viewer", "compiler_isa": "AVX2", "clock_origin": metadata,
                     "events": [t for t in viewer["trace"] if t["kind"] == "phase"]}
            result = summarize(session, output, probe)
            result = caller_counts(session, viewer, result)
            (output / "cpu-analysis.json").write_text(json.dumps(result, indent=2))
            print(case + " phases: " + str(len(result["stages"])), flush=True)
    vtune = Path(r"C:\Program Files (x86)\Intel\oneAPI\vtune\2025.3\bin64\vtune.exe")
    directory = root / "deep/vtune"
    for report, grouping in (("summary", None), ("hotspots", "function"), ("hotspots", "thread")):
        name = report if grouping is None else report + "-" + grouping
        command = [str(vtune), "-report", report, "-r", str(directory / "vtune"), "-format", "csv"]
        if grouping:
            command += ["-group-by", grouping]
        with (directory / (name + ".csv")).open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
        print("VTune " + name + ": " + str(result.returncode), flush=True)


if __name__ == "__main__":
    main()
