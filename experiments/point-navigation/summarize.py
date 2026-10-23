"""Archive completed point-render campaigns and retain independent round ranges."""
import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics


def aggregate(report):
    assert report["status"]=="completed"
    assert not report["guard"]["violations"]
    assert len({r["condition"] for r in report["rounds"]})==1,"Do not combine different scene conditions"
    result=[]
    for size in sorted({r["size"] for r in report["rounds"]}):
        rows=[r for r in report["rounds"] if r["size"]==size]
        assert len({r["round"] for r in rows})==len(rows),"Duplicate round"
        def summary(phase,metric):
            values=[r["summary"][phase][metric] for r in rows]
            medians=[v["median"] for v in values]
            assert all(math.isfinite(v[key]) for v in values for key in ("median","p95")),"Invalid timing"
            return dict(median=statistics.median(medians), minimum=min(medians), maximum=max(medians),
                worst_p95=max(v["p95"] for v in values), round_medians=medians)
        detail=[]
        for row in rows:
            previous=None
            for event in row["navigation"]["events"]:
                if previous is not None and event["camera"]!=previous and event["points"]["submittedCount"]:
                    detail.append(event["points"]["submittedCount"])
                previous=event["camera"]
        result.append(dict(size=size, rounds=len(rows),
            full_gpu_ms=summary("full","raster_frame_gpu_ms"),
            full_raster_ms=summary("full","point_raster_ms"),
            navigation_gpu_ms=summary("navigation","raster_frame_gpu_ms"),
            navigation_raster_ms=summary("navigation","point_raster_ms"),
            cached_gpu_ms=summary("cached","gpu_ms"),
            full_cpu_ms=summary("full","main_thread_work_ms"),
            navigation_submitted_points=statistics.median(detail) if detail else None))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("campaigns",type=Path,nargs="+")
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    summary={}
    for directory in args.campaigns:
        path=directory/"measurements.json"
        raw=path.read_bytes()
        report=json.loads(raw)
        rows=aggregate(report)
        name=directory.name.removeprefix("point-navigation-")
        archive=args.output/(name+".json.gz")
        archive.write_bytes(gzip.compress(raw,compresslevel=6,mtime=0))
        summary[name]=dict(model=report["model"], model_sha256=report["model_sha256"],
            points_only=report.get("points_only",False), zoom=report["zoom"],
            binary_sha256=report["executable_sha256"], shader_sha256=report.get("runtime_shader_sha256",{}),
            source_sha256=report["source_sha256"], guard=report["guard"],
            archive=archive.name, archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(), rows=rows)
        print(name,[(r["size"],round(r["full_gpu_ms"]["median"],3),round(r["navigation_gpu_ms"]["median"],3),
            r["navigation_submitted_points"]) for r in rows])
    (args.output/"summary.json").write_text(json.dumps(summary,indent=2),encoding="utf-8")


if __name__=="__main__":
    main()
