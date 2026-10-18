# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6"]
# ///
"""Measure acknowledged detector work in a real, isolated headless viewer."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

import psutil

DETECTORS = ["boundary_edges","non_manifold_edges","inconsistently_oriented_tris","duplicate_points",
             "duplicate_tris","degenerate_tris","non_manifold_vertices","holes","fins"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable",type=Path); parser.add_argument("model",type=Path); parser.add_argument("output",type=Path)
    parser.add_argument("--runs",type=int,default=3); parser.add_argument("--analysis-mode",choices=("whole","per_volume"),default="whole")
    args = parser.parse_args(); exe = args.executable.resolve(strict=True); model = args.model.resolve(strict=True)
    output = args.output.resolve(); output.mkdir(parents=True)
    instance = "detector-perf-" + uuid.uuid4().hex[:12]
    result = {"instance":instance,"model":str(model),"executable_sha256":hashlib.sha256(exe.read_bytes()).hexdigest(),
        "script_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "commit":subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
        "power_plan":subprocess.check_output(["powercfg","/getactivescheme"],text=True).strip(),
        "utc":datetime.now(timezone.utc).isoformat(),
        "analysis_mode":args.analysis_mode,"records":[],
        "scope":"Headless application and CLI acknowledgments, including snapshot preparation, scheduling and upload. Timings exclude initial model load unless labeled load. The two-sided comparison uses the same mesh on A and B."}
    def save():
        (output / "application.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="woby-detector-app-") as directory, (output / "viewer.log").open("w",encoding="utf-8") as log:
        root = Path(directory).resolve()
        startup = subprocess.STARTUPINFO(); startup.dwFlags = subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
        startup_command = [str(exe),"--headless","--instance",instance,"--log-performance",
            "--log-level","info","--log-file",str(output / "performance.log")]
        result["command"] = startup_command; save()
        viewer = subprocess.Popen(startup_command,
            cwd=root,stdout=log,stderr=log,startupinfo=startup)
        process = psutil.Process(viewer.pid)
        result["viewer_pid"] = viewer.pid; result["affinity"] = process.cpu_affinity(); save()
        def ctl(*command,allow_error=False):
            reply = subprocess.run([str(exe),"ctl","--instance",instance,*map(str,command),"--timeout","600","--json"],
                cwd=root,text=True,capture_output=True,timeout=650)
            try:
                value = json.loads(reply.stdout)
            except json.JSONDecodeError:
                raise RuntimeError(f"No JSON for {command}: {reply.stdout} {reply.stderr}")
            if reply.returncode and not allow_error:
                raise RuntimeError(f"Command failed: {command}: {value}")
            return value
        def measured(name,run,work):
            before = process.cpu_times(); start = time.perf_counter(); memory_before = process.memory_info()
            value = work(); elapsed = (time.perf_counter()-start)*1000; after = process.cpu_times(); memory = process.memory_info()
            record = {"name":name,"run":run,"wall_ms":elapsed,"cpu_ms":(after.user+after.system-before.user-before.system)*1000,
                "rss_begin":memory_before.rss,"rss_end":memory.rss,"lifetime_peak_rss":getattr(memory,"peak_wset",None),"result":value}
            result["records"].append(record); save(); print(json.dumps({"name":name,"run":run,"wall_ms":elapsed}),flush=True)
            return value
        try:
            deadline = time.monotonic()+60
            while True:
                if viewer.poll() is not None:
                    raise RuntimeError("Viewer exited at startup")
                reply = subprocess.run([str(exe),"ctl","instances","--json"],capture_output=True,text=True,timeout=10)
                instances = json.loads(reply.stdout)
                if any(v["id"] == instance and v["ready"] for v in instances):
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError("Viewer startup timed out")
                time.sleep(.1)
            measured("load",0,lambda:ctl("model","add",model))
            source = next(v["id"] for v in ctl("objects")["objects"] if v["kind"] == "file")
            ctl("model","set",source,"--analysis-mode",args.analysis_mode)
            result["source"] = ctl("object",source); save()
            for run in range(args.runs):
                analysis = None
                def automatic():
                    nonlocal analysis
                    created = ctl("analysis","create","--a",source,"--name",f"Detector sample {run}")
                    analysis = created["target"]
                    deadline = time.monotonic()+600
                    polls = 0
                    while True:
                        polls += 1
                        page = ctl("analysis","findings",analysis,"--side","a","--detector","fins","--limit","1",allow_error=True)
                        if page.get("metadata",{}).get("status") in ("complete","partial","unavailable"):
                            counts = {key:ctl("analysis","findings",analysis,"--side","a","--detector",key,"--limit","1",allow_error=True) for key in DETECTORS}
                            if all(value.get("metadata",{}).get("status") in ("complete","partial","unavailable") for value in counts.values()):
                                return {"target":analysis,"polls":polls,"detectors":counts}
                        if time.monotonic() > deadline:
                            raise TimeoutError(f"Detectors did not complete: {page}")
                        time.sleep(.05)
                automatic_result = measured("automatic_ready",run,automatic)
                result["records"][-1]["detectors"] = automatic_result["detectors"]; save()
                measured("quality_and_full_results",run,lambda:ctl("analysis","results",analysis))
                def intersections():
                    ctl("analysis","run",analysis,"--detector","self_intersections")
                    deadline = time.monotonic()+600; polls = 0
                    while True:
                        polls += 1
                        page = ctl("analysis","findings",analysis,"--side","a","--detector","self_intersections","--limit","1",allow_error=True)
                        if page.get("metadata",{}).get("status") in ("complete","partial","unavailable"):
                            return {"target":analysis,"polls":polls,"findings":page}
                        if time.monotonic() > deadline:
                            raise TimeoutError(f"Intersections did not complete: {page}")
                        time.sleep(.05)
                measured("manual_intersections_ready",run,intersections)
                measured("cached_results_query",run,lambda:ctl("analysis","results",analysis))
                measured("delete_analysis",run,lambda:ctl("analysis","delete",analysis))
            comparison = ctl("analysis","create","--type","surface_comparison","--a",source,"--b",source,"--name","Identical pair")["target"]
            measured("identical_pair_full_results",0,lambda:ctl("analysis","results",comparison))
            measured("difference_identical_pair",0,lambda:ctl("analysis","difference-create",comparison,allow_error=True))
            measured("delete_comparison",0,lambda:ctl("analysis","delete",comparison))
            result["status"] = "complete"; save()
        except Exception as error:
            result["status"] = "error"; result["error"] = str(error); save(); raise
        finally:
            if viewer.poll() is None:
                try:
                    ctl("quit","--on-dirty","discard",allow_error=True)
                    viewer.wait(timeout=20)
                except Exception:
                    viewer.kill(); viewer.wait()


if __name__ == "__main__":
    main()
