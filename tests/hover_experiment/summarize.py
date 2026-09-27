"""Summarize timing and correctness without mixing GPU frame IDs or warmup."""
import argparse
import json
from pathlib import Path
import statistics


def distribution(values):
    if not values:
        return None
    values=sorted(values)
    def percentile(p):
        position=(len(values)-1)*p
        lo=int(position)
        return values[lo]+(values[min(lo+1,len(values)-1)]-values[lo])*(position-lo)
    return dict(n=len(values),p50=statistics.median(values),p95=percentile(.95),max=max(values))


def enrich(run):
    raw=json.loads(Path(run["hover_path"]).read_text())
    result=dict(model=run["model"],round=run["round"],metadata=raw["metadata"],scenarios=[])
    for index,scenario in enumerate(run["scenarios"]):
        if scenario["name"]=="prime":
            continue
        rows=[e for e in raw["events"] if e["scenario"]==index and e["measured"] and e["picker"]!="gpu_times"]
        completed=[e for e in raw["completions"] if e["scenario"]==index and e["measured"]]
        latency_completed=completed
        if rows and not run["metadata"].get("validate"):
            # Later scenarios can intentionally block the CPU for >1 second.
            # Exclude the tail so those transitions cannot inflate readback latency.
            latency_completed=[e for e in completed if e["frame"]<=rows[-1]["frame"]-8]
        item=dict(scenario)
        item["query_cpu_ms"]=distribution([e["cpu_ms"] for e in rows])
        cpu_hits=[e for e in rows if e.get("picker")=="cpu" and e.get("rank",4294967295)!=4294967295]
        item["cpu_hit_queries"]=len(cpu_hits)
        item["cpu_queries"]=sum(e.get("picker")=="cpu" for e in rows)
        item["cpu_hit_ms"]=distribution([e["cpu_ms"] for e in cpu_hits])
        item["points_tested"]=distribution([e["points"] for e in rows])
        item["nodes_tested"]=distribution([e["nodes"] for e in rows])
        item["latency_ms"]=distribution([e["latency_ms"] for e in latency_completed])
        item["latency_frames"]=distribution([e["latency_frames"] for e in latency_completed])
        gpu={}
        if rows:
            low,high=rows[0]["frame"]+8,rows[-1]["frame"]-8
            for e in raw["events"]:
                if e["picker"]=="gpu_times":
                    for v in (60,61,62):
                        frame=e.get("view_frame_"+str(v),-1)
                        if low<=frame<=high and e.get("view_"+str(v),0)>0:
                            gpu.setdefault(frame,{})[v]=e["view_"+str(v)]
        item["picking_gpu_ms"]=distribution([sum(t.values()) for t in gpu.values() if len(t)==3])
        validation=[e for e in raw["completions"] if e["scenario"]==index and "expected_rank" in e]
        item["validation"]={"queries":len(validation),"exact_rank_matches":sum(e["rank"]==e["expected_rank"] for e in validation),
            "hit_presence_matches":sum((e["rank"]==4294967295)==(e["expected_rank"]==4294967295) for e in validation),
            "max_depth_error":max((abs(e["depth"]-e["expected_depth"]) for e in validation if e["rank"]!=4294967295 and e["expected_rank"]!=4294967295),default=0)}
        result["scenarios"].append(item)
    # Deterministic validation runs use exactly the same camera/pointer sequence.
    gpu_sequences={}
    for index,s in enumerate(run["scenarios"]):
        setting=s["settings"]
        if setting.get("picker") in ("gpu_flat","gpu_cluster"):
            events=[e for e in raw["completions"] if e["scenario"]==index]
            gpu_sequences[(setting["case"],setting["picker"])]=[e["rank"] for e in events]
    result["gpu_culling_validation"]=[]
    if run["metadata"].get("validate"):
        for (case,mode),seq in gpu_sequences.items():
            if mode=="gpu_cluster" and (case,"gpu_flat") in gpu_sequences:
                ref=gpu_sequences[(case,"gpu_flat")]
                result["gpu_culling_validation"].append(dict(case=case,queries=len(seq),reference_queries=len(ref),
                    exact_matches=sum(a==b for a,b in zip(seq,ref)),identical=seq==ref))
    return result


def main():
    p=argparse.ArgumentParser()
    p.add_argument("inputs",type=Path,nargs="+")
    p.add_argument("--output",type=Path,required=True)
    args=p.parse_args()
    rows=[enrich(json.loads(line)) for source in args.inputs for line in source.read_text().splitlines()]
    args.output.write_text(json.dumps(rows,indent=2),encoding="utf-8")
    for row in rows:
        print(row["model"],row["metadata"])
        for s in row["scenarios"]:
            latency=s["latency_ms"]; gpu=s["picking_gpu_ms"]
            print(s["name"],round(s["fps"],2),round(s["stages"]["hover_pick"],4),
                  "GPU",round(gpu["p50"],4) if gpu else None,"latency",round(latency["p50"],2) if latency else None,s["validation"])
        print("GPU culling validation",row["gpu_culling_validation"])


if __name__=="__main__":
    main()
