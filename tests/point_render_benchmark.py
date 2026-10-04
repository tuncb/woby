# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil>=6"]
# ///
"""Serial measurements of the integrated desktop point renderer (opt-in).

Uses the app's frame recorder and RPC camera controls. The hidden desktop window
retains normal frame pacing; GPU time and observed frame intervals are reported
separately. PNG exports always contain full source detail and are outside timing.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import threading
import time
import urllib.request
import uuid

import psutil


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def summarize(record):
    rows = record["events"]
    raster = [r for r in rows if r["points"].get("gpuRasterCount",0)]
    intervals = [b["completedMilliseconds"]-a["completedMilliseconds"] for a,b in zip(rows,rows[1:])]
    def values(data):
        ordered = sorted(data)
        return dict(median=statistics.median(ordered), p95=ordered[math.ceil((len(ordered)-1)*.95)], maximum=max(ordered)) if ordered else {}
    return dict(frames=len(rows), frame_interval_ms=values(intervals),
                raster_frames=len(raster),
                raster_frame_gpu_ms=values([r["gpuFrameMilliseconds"] for r in raster if r.get("gpuFrameMilliseconds") is not None]),
                point_raster_ms=values([r["points"]["gpuRasterMilliseconds"] for r in raster]),
                gpu_ms=values([r["gpuFrameMilliseconds"] for r in rows if r.get("gpuFrameMilliseconds") is not None]),
                frame_wall_ms=values([r["frameMilliseconds"] for r in rows]),
                cpu_submit_ms=values([r["cpuSubmitMilliseconds"] for r in rows]),
                stages_ms={name:values([r["stagesMilliseconds"][name] for r in rows])
                           for name in rows[0]["stagesMilliseconds"]} if rows else {},
                submitted_points=values([r["points"]["submittedCount"] for r in rows]),
                submitted_frames=sum(r["points"]["submittedCount"]>0 for r in rows),
                navigation_frames=sum(r["points"]["navigation"] for r in rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--sizes", type=int, nargs="+", default=[1,4,8])
    parser.add_argument("--navigation-frames", type=int, default=96)
    parser.add_argument("--full-frames", type=int, default=12)
    parser.add_argument("--zoom", type=float, default=1)
    parser.add_argument("--visible-window", action="store_true")
    args = parser.parse_args()
    if args.rounds < 1 or args.navigation_frames < 1 or args.full_frames < 1 or not all(1 <= s <= 40 for s in args.sizes):
        parser.error("Rounds and frame counts must be positive; point sizes must be 1..40")
    if not math.isfinite(args.zoom) or args.zoom <= 0:
        parser.error("Zoom must be positive and finite")
    args.executable=args.executable.resolve(); args.model=args.model.resolve(); args.output=args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    repository=Path(__file__).resolve().parents[1]
    guard=load_module(repository/"experiments/mesh-overlays/run.py", "point_overlap_guard")
    overlap=guard.overlapping_workloads()
    if overlap: raise RuntimeError(f"Another workload is active: {overlap}")
    with args.model.open("rb") as source: model_hash=hashlib.file_digest(source,"sha256").hexdigest()
    report=dict(model=str(args.model), model_sha256=model_hash,
                executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                source_revision=subprocess.check_output(["git","rev-parse","HEAD"],cwd=repository,text=True).strip(),
                source_sha256={str(p.relative_to(repository)):hashlib.sha256(p.read_bytes()).hexdigest()
                    for pattern in ("src/*point*.*","shaders/native/*point*.*","src/graphics.cpp","src/scene_renderer.cpp")
                    for p in repository.glob(pattern)}, rounds=[], hidden_window=not args.visible_window,
                requested_drawable=[1280,720], zoom=args.zoom)
    instance="point-app-"+uuid.uuid4().hex[:12]
    command=[str(args.executable),"--instance",instance,"--drawable-size","1280x720",
             "--log-file",str(args.output/"viewer.log"),"--log-level","info"]
    if not args.visible_window: command.append("--hidden-window")
    stopped=threading.Event(); violations=[]; memory=[]
    with tempfile.TemporaryDirectory(prefix="point-app-") as directory, (args.output/"console.log").open("w",encoding="utf-8") as log:
        startup=subprocess.STARTUPINFO() if os.name=="nt" else None
        if startup:
            startup.dwFlags=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=1 if args.visible_window else 0
        process=subprocess.Popen(command,cwd=directory,stdout=log,stderr=log,startupinfo=startup,
                                 env=dict(os.environ,VK_LOADER_LAYERS_DISABLE="~implicit~"))
        started=time.monotonic()
        def monitor():
            probe=psutil.Process(process.pid)
            while not stopped.wait(.2) and process.poll() is None:
                try:
                    info=probe.memory_info(); private=getattr(info,"private",info.rss); available=psutil.virtual_memory().available
                    memory.append(dict(seconds=time.monotonic()-started,private=private,available=available))
                    overlap=guard.overlapping_workloads(process.pid)
                    reason="overlap" if overlap else "private memory" if private>38*2**30 else "available memory" if available<6*2**30 else "timeout" if time.monotonic()-started>1200 else None
                    if reason:
                        violations.append(dict(reason=reason,overlap=overlap)); process.terminate(); return
                except psutil.NoSuchProcess: return
        watcher=threading.Thread(target=monitor,daemon=True); watcher.start()
        request_id=0; record={}
        def rpc(method,params=None,timeout=120):
            nonlocal request_id
            request_id+=1; payload=dict(params or {})
            if method not in ("instance.info","command.get"): payload["timeoutSeconds"]=timeout
            request=urllib.request.Request(f"http://127.0.0.1:{record['port']}/rpc",
                data=json.dumps(dict(jsonrpc="2.0",id=request_id,method=method,params=payload)).encode(),
                headers={"Content-Type":"application/json","Authorization":"Bearer "+record["token"]})
            with urllib.request.urlopen(request,timeout=timeout+5) as response: value=json.load(response)
            if "error" in value: raise RuntimeError(f"{method}: {value['error']}")
            return value["result"]
        try:
            registry=Path(os.environ["LOCALAPPDATA"])/"woby/instances"/("instance-"+instance+".json")
            deadline=time.monotonic()+45
            while time.monotonic()<deadline:
                assert process.poll() is None,"Viewer exited during startup"
                try:
                    record=json.loads(registry.read_text(encoding="utf-8"))
                    if rpc("instance.info").get("ready"): break
                except (OSError,ValueError): pass
                time.sleep(.05)
            else: raise TimeoutError("Startup timed out")
            begin=time.monotonic(); loaded=rpc("model.add",{"path":str(args.model)},600)
            assert loaded["addedCount"]==1 and loaded["failedCount"]==0,loaded
            report["load_seconds"]=time.monotonic()-begin
            rpc("pane.set",{"visible":False,"propertiesVisible":False})
            for helper in ("grid","origin","dimensions"): rpc(helper+".set",{"visible":False})
            rpc("camera.view",{"preset":"isometric"}); rpc("camera.frame")
            if args.zoom!=1: rpc("camera.dolly",{"factor":args.zoom})
            initial=rpc("camera.get")["camera"]
            placement={key:initial[key] for key in ("target","yawDegrees","pitchDegrees","rollDegrees","distance","nearPlane")}
            placement["fovDegrees"]=initial["verticalFovDegrees"]
            def move(frame):
                # Repeat the same twelve views so short full-source and longer
                # adaptive runs have matching angular coverage.
                rpc("camera.set",{"yawDegrees":initial["yawDegrees"]+6*math.sin(frame*math.tau/12),
                                  "pitchDegrees":initial["pitchDegrees"]+2*math.cos(frame*math.tau/12)})
            stats=rpc("stats"); report["stats"]=stats
            mesh=bool(stats.get("triangleCount"))
            rpc("render.set",{"target":"scene","solid":mesh,"triangles":mesh,"vertices":True,"adaptivePoints":True})
            for round_index in range(args.rounds):
                order=args.sizes[round_index%len(args.sizes):]+args.sizes[:round_index%len(args.sizes)]
                for size in order:
                    rpc("camera.set",placement); rpc("vertex-size.set",{"target":"scene","pixels":size})
                    rpc("render.set",{"target":"scene","adaptivePoints":False})
                    # Warm changing views, followed by all-source frame timing.
                    for frame in range(-4,0): move(frame)
                    rpc("performance.begin")
                    for frame in range(args.full_frames): move(frame)
                    full=rpc("performance.end")
                    rpc("render.set",{"target":"scene","adaptivePoints":True})
                    for frame in range(-12,0): move(frame)
                    rpc("performance.begin")
                    for frame in range(args.navigation_frames): move(frame)
                    navigation=rpc("performance.end")
                    rpc("performance.begin")
                    progress=[]; deadline=time.monotonic()+120
                    while time.monotonic()<deadline:
                        status=rpc("performance.get"); progress.append(status["points"])
                        assert status["points"]["active"],status["points"]
                        if status["points"]["sourceCount"] and status["points"]["refinedCount"]==status["points"]["sourceCount"]: break
                        time.sleep(.03)
                    else: raise TimeoutError("Points did not refine")
                    refinement=rpc("performance.end")
                    rpc("performance.begin"); time.sleep(.6); cached=rpc("performance.end")
                    sample=dict(round=round_index,size=size,full=full,navigation=navigation,refinement=refinement,cached=cached,progress=progress)
                    sample["summary"]={name:summarize(sample[name]) for name in ("full","navigation","refinement","cached")}
                    report["rounds"].append(sample)
                    (args.output/"measurements.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
                    print(json.dumps(dict(round=round_index,size=size,summary=sample["summary"])),flush=True)
            report["final_performance"]=rpc("performance.get")
            rpc("screenshot.capture",{"path":str(args.output/"full-detail.png")},120)
            rpc("quit",{"onDirty":"discard"}); assert process.wait(timeout=20)==0
            report["status"]="completed"
        finally:
            stopped.set(); watcher.join(timeout=2)
            if process.poll() is None: process.kill(); process.wait(timeout=15)
            report["guard"]=dict(violations=violations,peak_private=max((r["private"] for r in memory),default=0),
                minimum_available=min((r["available"] for r in memory),default=0),elapsed_seconds=time.monotonic()-started)
            (args.output/"measurements.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
            assert not violations,violations


if __name__=="__main__": main()
