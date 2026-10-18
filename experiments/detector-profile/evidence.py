"""Export compact, portable evidence from completed local captures."""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
from pathlib import Path
import statistics
import zipfile

from summarize import summarize


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path,value):
    path.write_text(json.dumps(value,indent=2),encoding="utf-8")


def csv_write(path,rows):
    if not rows:
        return
    keys = list(dict.fromkeys(k for row in rows for k in row))
    with path.open("w",newline="",encoding="utf-8") as stream:
        writer = csv.DictWriter(stream,fieldnames=keys); writer.writeheader()
        writer.writerows({key:json.dumps(value,separators=(",",":")) if isinstance(value,(dict,list)) else value
            for key,value in row.items()} for row in rows)


def bucket_quantile(histogram,fraction):
    total = sum(histogram); accumulated = 0
    for i,count in enumerate(histogram):
        accumulated += count
        if total and accumulated >= fraction*total:
            return [0,0] if i == 0 else [2**(i-1),2**i-1 if i < 31 else None]
    return None


def export(root,destination):
    destination.mkdir(parents=True,exist_ok=True)
    summary = summarize(root); write(destination / "summary.json",summary)
    csv_write(destination / "timings.csv",summary["rows"])
    hardware_rows = []; hardware_summary = []; query_rows = []; cancellations = []; phases = []
    predicates = []; power_rows = []; application_rows = []; detector_rows = []
    for case in summary["cases"]:
        name = case["name"]; operations = [e for e in case["events"] if e["name"].startswith("op.")]
        for event in case["events"]:
            parent = next((p["name"] for p in operations if p["start_ms"] <= event["start_ms"] and p["end_ms"] >= event["end_ms"]),None)
            phases.append({"case":name,"operation":parent,"name":event["name"],"start_ms":event["start_ms"],
                "wall_ms":event["wall_ms"],"cpu_ms":event["cpu_ms"],"caller_cpu_ms":event.get("caller_cpu_ms"),
                "user_ms":event.get("user_ms"),"kernel_ms":event.get("kernel_ms"),
                "os_process_cycles":event.get("cycles"),"page_faults":event.get("page_faults"),
                "read_bytes":event.get("read_bytes"),"write_bytes":event.get("write_bytes"),
                "average_logical_cpus":event["average_logical_cpus"],"working_set_end":event["working_set_end"],
                "commit_end":event["commit_end"],"data":json.dumps(event.get("data",{}),separators=(",",":"))})
            if "classifier_calls" in event.get("data",{}):
                predicates.append({"case":name,"operation":parent,"start_ms":event["start_ms"],
                    "wall_ms":event["wall_ms"],**event["data"]})
            if event["name"] == "distance.queries" and "queries" in event.get("data",{}):
                data = event["data"]; n = data["queries"]
                query_rows.append({"case":name,"direction":sum(row["case"] == name for row in query_rows),
                    "triangles_per_side":case["mesh"]["triangles"],"shift":case["mesh"]["shift"],
                    "batch_stride":data["batch_stride"],"queries":n,"mean_stack_visits":data["stack_visits"]/n if n else None,
                    "mean_triangle_tests":data["triangle_tests"]/n if n else None,"mean_internal_nodes":data["internal_nodes"]/n if n else None,
                    "zero_hint_percent":data["zero_distance_hints"]/n*100 if n else None,"maximum_stack_visits":data["maximum_stack_visits"],
                    "visit_p50_bucket":bucket_quantile(data.get("visit_histogram",[]),.5),
                    "visit_p95_bucket":bucket_quantile(data.get("visit_histogram",[]),.95),
                    "visit_p99_bucket":bucket_quantile(data.get("visit_histogram",[]),.99),
                    "triangle_p95_bucket":bucket_quantile(data.get("triangle_histogram",[]),.95)})
        for sample in case["records"]:
            if sample["operation"] == "auto" and sample["status"] == "ok":
                names = ("boundary_edges","non_manifold_edges","winding","duplicate_points","duplicate_triangles",
                    "degenerate_triangles","non_manifold_vertices","holes","fins")
                for detector,counts in zip(names,sample.get("result",{}).get("detector_counts",[])):
                    detector_rows.append({"case":name,"run":sample["run"],"detector":detector,
                        "side_a_count":counts[0],"side_b_count":counts[1],"tested_side":"a"})
            if "cancel_requested_ms" in sample:
                cancellations.append({"case":name,"operation":sample["operation"],"run":sample["run"],"status":sample["status"],
                    "requested_ms":sample["cancel_requested_ms"],"response_ms":sample.get("cancel_response_ms"),"error":sample.get("error")})
        path = root / name / "hardware.json"
        if path.exists():
            hardware = read(path)
            for stage in hardware["stages"]:
                base = {"case":name,"profile":hardware["profile"],"stage":stage["name"],"start_ms":stage["start_ms"],"wall_ms":stage["wall_ms"],
                    "triangles_per_input":case["mesh"]["triangles"],"compiler_overlap_observed":case["compiler_overlap_observed"],
                    "timeline_window_ms":hardware["timeline_window_ms"]}
                hardware_summary.append({**base,**{k:v for k,v in stage.items() if k.endswith("estimate")},
                    "estimated_instructions_per_input_triangle":stage.get("estimated_instructions_per_input_triangle"),
                    "cycle_samples":stage["counters"].get("076:00",{}).get("samples"),
                    "pdb_cycle_coverage_percent":stage.get("pdb_cycle_coverage_percent")})
                for key,event in stage["counters"].items():
                    hardware_rows.append({**base,"event_mask":key,**event,
                        "events_per_1000_instructions":stage.get("events_per_1000_instructions",{}).get(key),
                        "events_per_1000_cycles":stage.get("events_per_1000_cycles",{}).get(key),
                        "alignment_minus16ms_percent":stage["alignment_shift_-16ms_percent"][key],
                        "alignment_plus16ms_percent":stage["alignment_shift_16ms_percent"][key]})
        path = root / name / "power.json"
        if path.exists():
            for phase,columns in read(path)["phases"].items():
                for column,values in columns.items():
                    if values:
                        power_rows.append({"case":name,"phase":phase,"counter":column,**values})
    for path in sorted(root.glob("*/application.json")):
        application = read(path)
        for record in application["records"]:
            application_rows.append({"case":path.parent.name,"analysis_mode":application["analysis_mode"],
                "capture_status":application.get("status","incomplete"),
                **{k:v for k,v in record.items() if k not in ("result","detectors")}})
    for stem,rows in (("phases",phases),("hardware",hardware_rows),("hardware-summary",hardware_summary),
                      ("queries",query_rows),("predicates",predicates),("power",power_rows),
                      ("detectors",detector_rows),("application",application_rows),("cancellation",cancellations)):
        csv_write(destination / (stem+".csv"),rows)
        if stem in ("hardware-summary","queries","predicates","power","detectors","application","cancellation"):
            write(destination / (stem+".json"),rows)
    # Preserve the source files needed to audit summary tables without copying
    # binaries, models, third-party event XML, or multi-gigabyte profiler databases.
    manifest = []
    allowed = {"metadata.json","probe.json","resources.json","hardware.json","power.json","console.log","translation.log","session.uprof","analysis-notes.json",
        "application.json","performance.log","viewer.log","models.json","host.json","topology.json","power-plan.json",
        "build-configuration.json",
        "uprof-system.txt","uprof-version.json","wpr-sources.json","wpr-status.json","wpr-attempt.txt",
        "memory-modules.json","selected-events.json","event-definitions.json","vtune-hotspots.csv","vtune-summary.txt",
        "uprof-sources.json","equivalence.json","code-audit.json","postprocess.json",
        "processor-power-settings.txt","os-frequency.json","process-power-policy.json","collection-pause.json","collection-pause-window-alignment.json","collection-pause-external-build.json",
        "validation.json","build-release-final.log","unit-tests-release-final.log","probe-contract-final.log"}
    with zipfile.ZipFile(destination / "raw-evidence.zip","w",compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            symbol_file = "symbols-" in path.relative_to(root).parts[0]
            symbol_evidence = symbol_file and (path.name in ("manifest.json","dependencies.json")
                or "harness" in path.parts or path.suffix in (".cpp",".h",".py",".txt"))
            validation_evidence = path.relative_to(root).parts[0] == "validation" and path.suffix in (".json",".log",".txt")
            if not symbol_evidence and not validation_evidence and path.name not in allowed and not (path.suffix == ".csv" and "timechart" in path.parts):
                continue
            relative = path.relative_to(root).as_posix(); data = path.read_bytes()
            archive.writestr(relative,data)
            manifest.append({"path":relative,"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest()})
    write(destination / "evidence-manifest.json",{"local_root":str(root),"files":manifest,
        "archive_sha256":hashlib.sha256((destination / "raw-evidence.zip").read_bytes()).hexdigest()})
    print(json.dumps({"cases":len(summary["cases"]),"timing_rows":len(summary["rows"]),"hardware_rows":len(hardware_rows),
        "query_rows":len(query_rows),"raw_files":len(manifest)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("root",type=Path); parser.add_argument("output",type=Path)
    args = parser.parse_args(); export(args.root.resolve(),args.output.resolve())
