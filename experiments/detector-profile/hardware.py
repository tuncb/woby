# /// script
# requires-python = ">=3.11"
# dependencies = ["duckdb>=1.3", "pefile>=2024", "capstone>=5"]
# ///
"""Align native AMD PMC samples to probe scopes and preserve hotspot evidence."""
import argparse
from bisect import bisect_right
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def timeline_resolution(metadata):
    entries = [row for row in metadata.get("translation",[]) if row.get("category") == 1]
    if len(entries) != 1 or not isinstance(entries[0].get("aggregateInterval"),(int,float)) or entries[0]["aggregateInterval"] <= 0:
        raise ValueError("Missing or ambiguous CPU timeline aggregation interval")
    return entries[0]["aggregateInterval"]


def timeline_bounds(phase,offset_ms,window_ms):
    return ((offset_ms+phase["start_ms"])/window_ms,(offset_ms+phase["end_ms"])/window_ms)


def counter_metrics(counter):
    value = lambda key: counter.get(key,{}).get("estimated_events",0)
    cycles,instructions,branches,misses = [value(k) for k in ("076:00","0c0:00","0c2:00","0c3:00")]
    result = {"ipc_estimate":instructions/cycles if cycles else None,
        "branch_mispredict_percent_estimate":misses/branches*100 if branches and "0c3:00" in counter else None,
        "branch_mpki_estimate":misses/instructions*1000 if instructions and "0c3:00" in counter else None,
        "events_per_1000_instructions":{k:v["estimated_events"]/instructions*1000 for k,v in counter.items()
            if k not in ("076:00","0c0:00")} if instructions else {},
        "events_per_1000_cycles":{k:v["estimated_events"]/cycles*1000 for k,v in counter.items()
            if k not in ("076:00","0c0:00")} if cycles else {}}
    if "064:08" in counter and "064:f0" in counter:
        hits,miss = value("064:f0"),value("064:08")
        result["l2_data_request_miss_percent_estimate"] = 100*miss/(hits+miss) if hits+miss else None
    if "045:0f" in counter and "045:f0" in counter:
        hits,miss = value("045:0f"),value("045:f0")
        result["dtlb_walk_share_of_l1_misses_percent_estimate"] = 100*miss/(hits+miss) if hits+miss else None
    if all(k in counter for k in ("043:01","043:02","043:08")):
        fills = sum(value(k) for k in ("043:01","043:02","043:08"))
        result["observed_demand_fill_source_percent_estimate"] = {
            k:value(k)/fills*100 if fills else None for k in ("043:01","043:02","043:08")}
    return result


def analyze(directory):
    import capstone
    import duckdb
    import pefile
    probe = load(directory / "probe.json"); metadata = load(directory / "session/session.uprof")
    capture = load(directory / "metadata.json"); symbols = Path(capture["symbols"])
    exe = next(symbols.glob("*.exe"))
    if hashlib.sha256(exe.read_bytes()).hexdigest() != capture["executable_sha256"]:
        raise ValueError("Captured executable hash differs")
    database = duckdb.connect(str(directory / "session/cpu.db"), read_only=True)
    schemas = {row[0]: [r[0] for r in database.execute(f'describe "{row[0]}"').fetchall()]
               for row in database.execute("show tables").fetchall()}
    events = metadata["collection"]["events"]["pmcEvents"]["pmcEventList"]
    if len(events) > 6:
        raise ValueError("Capture exceeds six available PMCs")
    offset = (probe["clock_origin"]["steady_epoch_nanoseconds"]-metadata["collection"]["schedule"]["start"])/1e6
    window_ms = timeline_resolution(metadata)
    expressions = ",".join(f'sum("event#0x{e["encodedEventId"]:x}")' for e in events)
    process_ids = database.execute("select distinct processId from UnifiedSampleSeries").fetchall()
    native_pid = probe["clock_origin"]["pid"]
    selected_ids = [row[0] for row in process_ids if row[0] == native_pid]
    if not selected_ids and "Process" in schemas:
        for column in ("pid", "processId", "osProcessId"):
            if column in schemas["Process"]:
                selected_ids = [r[0] for r in database.execute(f'select id from Process where "{column}"=?',[native_pid]).fetchall()]
                if selected_ids:
                    break
    if not selected_ids:
        if len(process_ids) == 1:
            selected_ids = [process_ids[0][0]]
        else:
            raise ValueError(f"Cannot select native target PID {native_pid}: {process_ids}")
    ids = ",".join(str(int(n)) for n in selected_ids)
    extent = database.execute(f"select min(windowId),max(windowId) from UnifiedSampleSeries where processId in ({ids})").fetchone()
    where = f"where processId in ({ids}) and windowId>=? and windowId<?"
    def counts(begin, end):
        values = database.execute("select "+expressions+" from UnifiedSampleSeries "+where,[begin,end]).fetchone()
        return {f'{event["hwId"]:03x}:{event["mask"]:02x}': {"samples":value or 0,"interval":event["interval"],
                    "estimated_events":(value or 0)*event["interval"]} for event,value in zip(events,values)}
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("production_tools",root / "experiments/obj-profile/production_tools.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    pe = pefile.PE(str(exe),fast_load=True); binary = pe.get_memory_mapped_image()
    function_map = module.map_functions(next(symbols.glob("*.map")).read_text(),pe.OPTIONAL_HEADER.ImageBase)
    addresses = list(function_map); resolved = {}
    def function_name(rva):
        index = bisect_right(addresses,rva)-1
        if index < 0:
            return "unresolved"
        address = addresses[index]
        if address not in resolved:
            resolved[address] = " | ".join(module.undecorate(name) for name,obj in function_map[address])
        return resolved[address]
    decoder = capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    stages = []
    for phase in probe["events"]:
        if phase["wall_ms"] < 100:
            continue
        begin, end = timeline_bounds(phase,offset,window_ms)
        counter = counts(begin,end)
        if phase["name"].startswith("op.") and phase["cpu_ms"] >= 1000 and not counter["076:00"]["samples"]:
            raise ValueError(f"No cycle samples for a CPU-active operation: {phase['name']}; check timeline alignment")
        event_value = lambda key: counter.get(key,{}).get("estimated_events",0)
        cycles,instructions,branches,misses = [event_value(k) for k in ("076:00","0c0:00","0c2:00","0c3:00")]
        stage = {"name":phase["name"],"start_ms":phase["start_ms"],"end_ms":phase["end_ms"],
            "wall_ms":phase["wall_ms"],"cpu_ms":phase["cpu_ms"],"average_logical_cpus":phase["average_logical_cpus"],
            "counters":counter,**counter_metrics(counter),
            "estimated_instructions_per_input_triangle":instructions/probe["mesh"]["triangles"] if instructions else None}
        for shift in (-16,16):
            shifted = counts(begin+shift/window_ms,end+shift/window_ms)
            stage[f"alignment_shift_{shift}ms_percent"] = {key:(shifted[key]["estimated_events"]/value["estimated_events"]-1)*100
                if value["estimated_events"] else None for key,value in counter.items()}
        rows = database.execute(f'''select m.path,s.moduleRVA,sum(s."event#0x1000076") cycles,
            sum(s."event#0x10000c0") instructions from UnifiedSampleSeries s join Module m on s.moduleId=m.id
            where s.processId in ({ids}) and windowId>=? and windowId<?
            group by m.path,s.moduleRVA''',[begin,end]).fetchall()
        functions = defaultdict(lambda:[0,0]); locations = []
        for path,rva,cycle_samples,instruction_samples in rows:
            own = Path(path).name.lower() == exe.name.lower()
            name = function_name(rva) if own else Path(path).name
            functions[name][0] += cycle_samples or 0; functions[name][1] += instruction_samples or 0
            if own and cycle_samples:
                decoded = next(decoder.disasm(binary[rva:rva+16],rva),None)
                locations.append({"rva":rva,"function":name,"cycle_samples":cycle_samples,
                    "instruction_samples":instruction_samples or 0,
                    "instruction": f"{decoded.mnemonic} {decoded.op_str}" if decoded else None})
        total = sum(value[0] for value in functions.values())
        stage["top_functions_map"] = [{"function":name,"cycle_samples":v[0],"cycle_percent":v[0]/total*100 if total else 0,
            "instruction_samples":v[1]} for name,v in sorted(functions.items(),key=lambda row:row[1][0],reverse=True)[:30]]
        native_functions = database.execute(f'''select f.name,m.path,f.startOffset,f.size,
            sum(s."event#0x1000076") cycles,sum(s."event#0x10000c0") instructions
            from UnifiedSampleSeries s join Module m on s.moduleId=m.id
            left join Function f on s.functionId=f.id
            where s.processId in ({ids}) and windowId>=? and windowId<?
            group by f.name,m.path,f.startOffset,f.size order by cycles desc''',[begin,end]).fetchall()
        stage["top_functions"] = [{"function":name or "unresolved", "module":path,"start_rva":rva,"size":size,
            "cycle_samples":c or 0,"cycle_percent":(c or 0)/total*100 if total else 0,"instruction_samples":i or 0}
            for name,path,rva,size,c,i in native_functions[:30]]
        stage["pdb_cycle_coverage_percent"] = sum((row[4] or 0) for row in native_functions if row[0])/total*100 if total else 0
        stage["top_instructions"] = sorted(locations,key=lambda row:row["cycle_samples"],reverse=True)[:30]
        # Attribute cache/branch/stall events as well as cycles. These are sampled
        # instruction pointers, not precise addresses of the load or branch.
        event_expressions = ",".join(f'sum(s."event#0x{e["encodedEventId"]:x}")' for e in events)
        event_locations = database.execute(f'''select f.name,m.path,s.moduleRVA,{event_expressions}
            from UnifiedSampleSeries s join Module m on s.moduleId=m.id
            left join Function f on s.functionId=f.id
            where s.processId in ({ids}) and windowId>=? and windowId<?
            group by f.name,m.path,s.moduleRVA''',[begin,end]).fetchall()
        stage["event_hotspots"] = {}
        for index,event in enumerate(events):
            key = f'{event["hwId"]:03x}:{event["mask"]:02x}'
            function_counts = defaultdict(int); instruction_rows = []
            for name,path,rva,*values in event_locations:
                count = values[index] or 0
                if not count:
                    continue
                function_counts[(name or "unresolved",path)] += count
                instruction_rows.append((count,name or "unresolved",path,rva))
            denominator = counter[key]["samples"]
            top_locations = []
            for count,name,path,rva in sorted(instruction_rows,reverse=True)[:10]:
                instruction = None
                if Path(path).name.lower() == exe.name.lower():
                    decoded = next(decoder.disasm(binary[rva:rva+16],rva),None)
                    instruction = f"{decoded.mnemonic} {decoded.op_str}" if decoded else None
                top_locations.append({"function":name,"module":path,"rva":rva,"samples":count,
                    "percent":count/denominator*100 if denominator else None,"instruction":instruction})
            stage["event_hotspots"][key] = {
                "functions":[{"function":name,"module":path,"samples":count,
                    "percent":count/denominator*100 if denominator else None}
                    for (name,path),count in sorted(function_counts.items(),key=lambda row:row[1],reverse=True)[:10]],
                "locations":top_locations}
        for column in ("coreId","threadId"):
            if column in schemas["UnifiedSampleSeries"]:
                distribution = database.execute(f'''select {column},sum("event#0x1000076") n
                    from UnifiedSampleSeries {where} group by {column} having n>0 order by n desc''',[begin,end]).fetchall()
                stage[column] = distribution
        if "thread_id" in phase.get("data",{}):
            thread = int(phase["data"]["thread_id"])
            values = database.execute("select "+expressions+" from UnifiedSampleSeries "+where+" and threadId=?",[begin,end,thread]).fetchone()
            stage["caller_thread_id"] = thread
            stage["caller_thread_counter_samples"] = {f'{event["hwId"]:03x}:{event["mask"]:02x}':value or 0
                for event,value in zip(events,values)}
        stages.append(stage)
    database.close()
    result = {"capture":directory.name,"profile":capture["profile"],"executable_sha256":capture["executable_sha256"],
        "analyzer_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "tool_versions":{"duckdb":duckdb.__version__,"pefile":pefile.__version__,"capstone":capstone.__version__},
        "probe_native_pid":native_pid,"selected_process_ids":selected_ids,"clock_offset_ms":offset,
        "timeline_window_ms":window_ms,"sample_extent_from_capture_start_ms":[v*window_ms if v is not None else None for v in extent],
        "alignment":"QPC metadata and the CPU translation aggregateInterval; +/-16 ms scope sensitivity retained. Sampling has skid; counts are period-scaled estimates.",
        "metric_limits":"Rates use only captured events. L2 request misses exclude L2 prefetches. Demand fill shares describe captured refill sources, not general cache hit rates. Dispatch events can overlap. Caller-only samples exclude worker threads and are not inclusive totals.",
        "scope_attribution":"All selected target-process threads during each wall interval. Concurrent stages overlap; a named substage can include samples from another active stage.",
        "function_resolution":"uProf Function records from symbols; matching linker-map resolution retained separately. Inlining, sampling skid and identical-code folding limit attribution.",
        "events":events,"schemas":schemas,"stages":stages}
    (directory / "hardware.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(json.dumps({"capture":directory.name,"stages":len(stages),"offset_ms":offset,
        "operations":[{k:s[k] for k in ("name","ipc_estimate","branch_mispredict_percent_estimate")} for s in stages if s["name"].startswith("op.")]}),flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("directories",nargs="+",type=Path)
    args = parser.parse_args()
    for directory in args.directories:
        analyze(directory.resolve())
