# /// script
# requires-python = ">=3.13"
# dependencies = ["psutil>=6", "Pillow>=11"]
# ///
"""Serial measurements of the integrated desktop point renderer (opt-in).

Uses the app's frame recorder and RPC camera controls with production pacing.
Hidden windows use 20 Hz; use --visible-window for desktop navigation measurements.
GPU time and RPC-driven frame intervals are reported separately. PNG exports
always contain full source detail and are outside timing.
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
from PIL import Image


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def summarize(record):
    rows = record["events"]
    raster = [r for r in rows if r["points"].get("gpuRasterCount",0)]
    submitted = [r for r in rows if r["points"]["submittedCount"]]
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
                scene_submit_work_ms=values([r["stagesMilliseconds"]["submit_scene"] for r in submitted]),
                main_thread_work_ms=values([sum(value for name,value in r["stagesMilliseconds"].items()
                                               if name!="graphics_frame") for r in submitted]),
                stages_ms={name:values([r["stagesMilliseconds"][name] for r in rows])
                           for name in rows[0]["stagesMilliseconds"]} if rows else {},
                submitted_points=values([r["points"]["submittedCount"] for r in rows]),
                submitted_frames=sum(r["points"]["submittedCount"]>0 for r in rows),
                navigation_frames=sum(r["points"]["navigation"] for r in rows))


def surface_fixture(camera, radius):
    """Two camera-facing sheets straddle the cloud without changing its camera."""
    def normalized(vector):
        length=math.sqrt(sum(value*value for value in vector))
        return [value/length for value in vector]
    def cross(a,b):
        return [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]]
    normal=normalized([a-b for a,b in zip(camera["eye"],camera["target"])])
    right=normalized(cross(camera["up"],normal)); up=cross(normal,right)
    lines=[]
    for sheet,depth in enumerate((-.65,.65)):
        for x,y in ((-1,-1),(1,-1),(1,1),(-1,1)):
            point=[camera["target"][i]+radius*(depth*normal[i]+.8*x*right[i]+.65*y*up[i]) for i in range(3)]
            lines.append("v "+" ".join(format(value,".9g") for value in point))
        first=4*sheet+1
        lines.extend((f"g sheet-{sheet}",f"f {first} {first+1} {first+2}",f"f {first} {first+2} {first+3}"))
    return "\n".join(lines)+"\n"


def validate_capture(record, source_count, drawable, viewport, cached=False):
    assert record["events"] and record["droppedFrames"]==0,"Missing or dropped frame samples"
    for environment in record["environments"]:
        assert environment["drawable"]==drawable,environment
        assert environment["viewport"]==viewport,environment
        assert environment["pacing"]["msaaSamples"]==4,environment
    for row in record["events"]:
        points=row["points"]
        assert points["active"] and points["sourceCount"]==source_count,points
        if cached:
            assert points["refinedCount"]==source_count and points["submittedCount"]==0,points


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
    parser.add_argument("--compare-transparent-surfaces", action="store_true")
    parser.add_argument("--opacity-checks", action="store_true")
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
                requested_drawable=[1280,720], zoom=args.zoom,
                compare_transparent_surfaces=args.compare_transparent_surfaces,
                opacity_checks=args.opacity_checks,
                runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
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
            report["initial_camera"]=initial
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
            cloud_id=loaded["addedIds"][0]; surface_id=None
            if args.compare_transparent_surfaces:
                surface_path=args.output/"transparent-sheets.obj"
                surface_path.write_text(surface_fixture(initial,stats["bounds"]["radius"]),encoding="utf-8")
                surface=rpc("model.add",{"path":str(surface_path)})
                assert surface["addedCount"]==1 and surface["failedCount"]==0,surface
                surface_id=surface["addedIds"][0]
                rpc("render.set",{"target":surface_id,"solid":True,"triangles":True,"vertices":True})
                rpc("color.set",{"target":surface_id,"rgb":[.1,.6,1]})
                rpc("opacity.set",{"target":surface_id,"value":.35})
                rpc("visibility.set",{"target":surface_id,"visible":False})
                report["surface_fixture"]=dict(path=str(surface_path),sha256=hashlib.sha256(surface_path.read_bytes()).hexdigest(),
                    triangles=4,vertices=8,opacity=.35,edges=True,markers=True)
            environment=rpc("performance.get")
            drawable=environment["drawable"]; viewport=environment["viewport"]
            assert drawable==dict(width=1280,height=720),drawable
            report["environment"]=environment
            conditions=["cloud-only","transparent-surfaces"] if surface_id else ["cloud-only"]
            for round_index in range(args.rounds):
                order=args.sizes[round_index%len(args.sizes):]+args.sizes[:round_index%len(args.sizes)]
                cases=[(size,condition) for index,size in enumerate(order)
                       for condition in (conditions[(round_index+index)%len(conditions):]+conditions[:(round_index+index)%len(conditions)])]
                for size,condition in cases:
                    if surface_id:
                        rpc("visibility.set",{"target":surface_id,"visible":condition=="transparent-surfaces"})
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
                    time.sleep(.25) # Drain delayed refinement timestamps before cached timing.
                    rpc("performance.begin"); time.sleep(.6); cached=rpc("performance.end")
                    source_count=progress[-1]["sourceCount"] if mesh else stats["pointCount"]
                    for capture in (full,navigation,refinement): validate_capture(capture,source_count,drawable,viewport)
                    validate_capture(cached,source_count,drawable,viewport,cached=True)
                    sample=dict(round=round_index,size=size,condition=condition,full=full,navigation=navigation,
                                refinement=refinement,cached=cached,progress=progress)
                    sample["summary"]={name:summarize(sample[name]) for name in ("full","navigation","refinement","cached")}
                    if args.opacity_checks:
                        checks=[]
                        changes=[(cloud_id,value,"cloud") for value in (.35,0,1)]
                        if surface_id and condition=="transparent-surfaces":
                            changes.extend((surface_id,value,"surface") for value in (.6,.35))
                        for target,value,kind in changes:
                            rpc("performance.begin")
                            rpc("opacity.set",{"target":target,"value":value})
                            time.sleep(.08)
                            capture=rpc("performance.end")
                            validate_capture(capture,source_count,drawable,viewport,cached=True)
                            checks.append(dict(target=kind,opacity=value,capture=capture,summary=summarize(capture)))
                        sample["opacity_checks"]=checks
                    if round_index==args.rounds-1:
                        image=args.output/f"{condition}-{size}px.png"
                        rpc("screenshot.capture",{"path":str(image)},120)
                        sample["screenshot"]=image.name
                        if args.opacity_checks and size==4:
                            zero_image=args.output/f"{condition}-{size}px-zero-opacity.png"
                            rpc("opacity.set",{"target":cloud_id,"value":0})
                            rpc("screenshot.capture",{"path":str(zero_image)},120)
                            rpc("opacity.set",{"target":cloud_id,"value":1})
                            with Image.open(image) as opaque,Image.open(zero_image) as zero:
                                assert opaque.size==zero.size and opaque.convert("RGB").tobytes()==zero.convert("RGB").tobytes(),"Cloud opacity changed exported pixels"
                                sample["opacity_pixel_check"]=dict(unchanged=True,dimensions=opaque.size,
                                    opaque=image.name,zero=zero_image.name)
                    report["rounds"].append(sample)
                    (args.output/"measurements.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
                    print(json.dumps(dict(round=round_index,size=size,condition=condition,
                        full_gpu_ms=sample["summary"]["full"]["raster_frame_gpu_ms"].get("median"),
                        navigation_gpu_ms=sample["summary"]["navigation"]["raster_frame_gpu_ms"].get("median"),
                        navigation_gpu_p95_ms=sample["summary"]["navigation"]["raster_frame_gpu_ms"].get("p95"),
                        cached_gpu_ms=sample["summary"]["cached"]["gpu_ms"].get("median"),
                        cache_checks=len(sample.get("opacity_checks",[])))),flush=True)
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
