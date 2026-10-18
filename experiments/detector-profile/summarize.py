"""Summarize completed captures without replacing the per-run evidence."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def summarize(root):
    rows = []; cases = []; missing_probe = []
    for directory in sorted(root.iterdir()):
        if not directory.is_dir() or not (directory / "metadata.json").exists():
            continue
        metadata = read(directory / "metadata.json")
        if "exit_code" not in metadata:
            continue
        if not (directory / "probe.json").exists():
            missing_probe.append({"name":directory.name,"metadata":metadata})
            continue
        probe = read(directory / "probe.json")
        resources = read(directory / "resources.json")
        samples = defaultdict(list); events = defaultdict(list)
        for sample in probe["samples"]:
            samples[sample["operation"]].append(sample)
        for event in probe["events"]:
            events[event["name"]].append(event)
        intervals = resources.get("external_cpu",[])
        duration = sum(r["interval_s"] for r in intervals)
        external = sum(p["cpu_s"] for r in intervals for p in r["busy"])
        busy = defaultdict(float)
        for interval in intervals:
            for process in interval["busy"]:
                busy[process["name"]] += process["cpu_s"]
        compiler_cpu = sum(value for name,value in busy.items() if name.lower() in ("cl.exe","clang.exe","clang-cl.exe","link.exe","gcc.exe","g++.exe"))
        build_cpu = compiler_cpu + sum(value for name,value in busy.items() if name.lower() in ("msbuild.exe","cmake.exe","ninja.exe"))
        notes_path = directory / "analysis-notes.json"
        notes = read(notes_path) if notes_path.exists() else {}
        gaps = sorted(b["elapsed_s"]-a["elapsed_s"] for a,b in zip(resources["samples"],resources["samples"][1:])
            if a.get("pid") == b.get("pid") and b["elapsed_s"] > a["elapsed_s"])
        case = {"name":directory.name,"metadata":metadata,"mesh":probe.get("mesh"),"options":probe["options"],
            "capture_complete":metadata["exit_code"] == 0 and not metadata.get("stop_reason"),
            "external_compiler_cpu_seconds_lower_bound":compiler_cpu,
            "external_build_cpu_seconds_lower_bound":build_cpu,
            "compiler_overlap_observed":compiler_cpu > 0,
            "analysis_notes":notes,
            "instrumented":probe["instrumented"],"external_average_cores_lower_bound":external/duration if duration else None,
            "experimental_change":probe.get("experimental_change"),
            "query_counters":probe.get("query_counters",False),"query_batch_stride":probe.get("query_batch_stride",1),
            "distance_statistics_valid":probe.get("distance_statistics_valid",True),
            "external_top_cpu_seconds":dict(sorted(busy.items(),key=lambda r:r[1],reverse=True)[:10]),
            "resource_sample_count":len(resources["samples"]),
            "resource_interval_median_s":statistics.median(gaps) if gaps else None,
            "resource_interval_p95_s":gaps[int(.95*(len(gaps)-1))] if gaps else None,
            "resource_interval_max_s":max(gaps) if gaps else None,
            "sampled_peak_rss_gib":max((s["rss"] for s in resources["samples"]),default=0)/1024**3,
            "sampled_peak_private_gib":max((s["private"] or 0 for s in resources["samples"]),default=0)/1024**3,
            "records":probe["samples"],"events":probe["events"]}
        cases.append(case)
        for operation, records in samples.items():
            timings = events["op."+operation]
            if len(timings) != len(records):
                raise ValueError(f"Scope/sample count differs: {directory.name}/{operation}")
            values = [e["wall_ms"] for e in timings]
            releases = events["release."+operation]
            errors = sorted({r.get("error","") for r in records if r["status"] != "ok"})
            first = records[0].get("result",{})
            intersections = first.get("surfaces",[{}])[0].get("intersection",{})
            scope_memory = []
            if "observer_start_qpc_ns" in metadata:
                offset_s = (probe["clock_origin"]["steady_epoch_nanoseconds"]-metadata["observer_start_qpc_ns"])/1e9
                for event in timings:
                    scope_memory.extend(s for s in resources["samples"]
                        if offset_s+event["start_ms"]/1000 <= s["elapsed_s"] <= offset_s+event["end_ms"]/1000)
            rows.append({"case":directory.name,"profile":metadata["profile"] or "none","instrumented":probe["instrumented"],
                "capture_complete":case["capture_complete"],"stop_reason":metadata.get("stop_reason"),
                "compiler_overlap_observed":case["compiler_overlap_observed"],
                "external_compiler_cpu_seconds_lower_bound":compiler_cpu,
                "external_build_cpu_seconds_lower_bound":build_cpu,
                "query_batch_stride":probe.get("query_batch_stride",1),"distance_statistics_valid":probe.get("distance_statistics_valid",True),
                "operation":operation,"triangles":probe["mesh"]["triangles"],"runs":len(values),"status":"error" if errors else "ok",
                "median_ms":statistics.median(values),"min_ms":min(values),"max_ms":max(values),
                "median_cpu_ms":statistics.median(e["cpu_ms"] for e in timings),
                "median_user_ms":statistics.median(e["user_ms"] for e in timings) if all("user_ms" in e for e in timings) else None,
                "median_kernel_ms":statistics.median(e["kernel_ms"] for e in timings) if all("kernel_ms" in e for e in timings) else None,
                "median_page_faults":statistics.median(e["page_faults"] for e in timings) if all("page_faults" in e for e in timings) else None,
                "median_average_logical_cpus":statistics.median(e["average_logical_cpus"] for e in timings),
                "median_commit_delta_mib":statistics.median((e["commit_end"]-e["commit_begin"])/1024**2 for e in timings),
                "lifetime_peak_rss_gib":max(e["process_peak_working_set"] for e in timings)/1024**3,
                "scope_memory_samples":len(scope_memory),
                "scope_sampled_peak_rss_gib":max(s["rss"] for s in scope_memory)/1024**3 if scope_memory else None,
                "scope_sampled_peak_private_gib":max((s["private"] or 0) for s in scope_memory)/1024**3 if scope_memory else None,
                "release_median_ms":statistics.median(e["wall_ms"] for e in releases) if releases else None,
                "intersection_candidates":intersections.get("candidate_tests") if operation.startswith("intersections") else None,
                "intersection_truncated":intersections.get("truncated") if operation.startswith("intersections") else None,
                "intersection_reason":intersections.get("reason") if operation.startswith("intersections") else None,
                "error":" | ".join(errors),"external_average_cores_lower_bound":case["external_average_cores_lower_bound"]})
    return {"rows":rows,"cases":cases,"completed_captures_without_probe":missing_probe}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("root",type=Path); parser.add_argument("--output",type=Path)
    args = parser.parse_args(); result = summarize(args.root.resolve())
    if args.output:
        args.output.mkdir(parents=True,exist_ok=True)
        (args.output / "summary.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
        if result["rows"]:
            with (args.output / "timings.csv").open("w",newline="",encoding="utf-8") as stream:
                writer = csv.DictWriter(stream,fieldnames=list(result["rows"][0])); writer.writeheader(); writer.writerows(result["rows"])
    for row in result["rows"]:
        print(f'{row["case"]:34} {row["operation"]:20} {row["median_ms"]:11.2f} ms  {row["status"]}')
