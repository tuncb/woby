"""Serial full-viewer experiments. No concurrent builds or other viewers during timing."""
import argparse
import csv
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import time


def scenarios(suite="core"):
    choices = [
        dict(name="solid_fit"),
        dict(name="solid_far", distance_factor=4),
        dict(name="solid_near", distance_factor=.5),
        dict(name="solid_offscreen", offscreen=True),
        dict(name="solid_orbit", motion="orbit", drag=True),
        dict(name="solid_pan", motion="pan", drag=True),
        dict(name="solid_zoom", motion="zoom"),
        dict(name="solid_no_msaa", msaa=False),
        dict(name="solid_1080p", width=1920, height=1080),
        dict(name="solid_vsync", vsync=True),
        dict(name="solid_pane", pane=True, helpers=True),
        dict(name="solid_selected", selected=True, inspector=True),
        dict(name="solid_alpha", opacity=.4),
        dict(name="edges", solid=False, edges=True),
        dict(name="solid_edges", edges=True),
        dict(name="points4", solid=False, vertices=True),
        dict(name="points_far", solid=False, vertices=True, distance_factor=4),
        dict(name="points_near", solid=False, vertices=True, distance_factor=.5),
        dict(name="points8", solid=False, vertices=True, point_size=8),
        dict(name="solid_points", vertices=True),
        dict(name="points_hover_static", solid=False, vertices=True, pointer="center"),
        dict(name="points_hover_move", solid=False, vertices=True, pointer="sweep"),
        dict(name="points_orbit", solid=False, vertices=True, motion="orbit", drag=True, pointer="center"),
        dict(name="points_zoom", solid=False, vertices=True, motion="zoom", pointer="center"),
        dict(name="points_keyboard", solid=False, vertices=True, motion="pan", pointer="center"),
        dict(name="hidden", visible=False),
        dict(name="points_zoom_no_hover", solid=False, vertices=True, motion="zoom"),
        dict(name="points_pan_drag", solid=False, vertices=True, motion="pan", drag=True, pointer="center"),
        dict(name="production_flags_pane", vsync=True, pane=True, helpers=True),
        dict(name="solid_zoom_pointer", motion="zoom", pointer="center"),
        dict(name="points_no_msaa", solid=False, vertices=True, msaa=False),
        dict(name="edges_no_msaa", solid=False, edges=True, msaa=False),
        dict(name="points8_alpha", solid=False, vertices=True, point_size=8, opacity=.4),
        dict(name="solid_zero_opacity", opacity=0),
        dict(name="points_depth_range", solid=False, vertices=True, near_fraction=.01),
        dict(name="points_far_depth_range", solid=False, vertices=True, near_fraction=.01, distance_factor=4),
        dict(name="solid_depth_range", near_fraction=.01),
        dict(name="control_points4", solid=False, vertices=True),
        dict(name="control_points_far", solid=False, vertices=True, distance_factor=4),
        dict(name="control_edges", solid=False, edges=True),
        dict(name="control_solid"),
    ]
    return choices[:26] if suite == "core" else choices[26:] if suite == "extra" else choices


def percentile(values, p):
    values = sorted(values)
    if not values:
        return None
    index = (len(values)-1)*p
    lo = int(index)
    return values[lo] + (values[min(lo+1, len(values)-1)]-values[lo])*(index-lo)


def summarize_run(output):
    meta = json.loads(output.with_suffix(".json").read_text())
    with output.with_suffix(".csv").open() as stream:
        rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(stream)]
    result = []
    for index, scenario in enumerate(meta["scenarios"]):
        measured = [r for r in rows if r["scenario"] == index and r["measured"] == 1]
        if len(measured) < meta["min_frames"]:
            raise ValueError(f"Missing measured frames for {scenario['name']}")
        # Timestamp results arrive asynchronously and may repeat. Attribute only
        # unique GPU IDs well inside this scenario's measured source-frame range.
        low, high = measured[0]["frame"]+8, measured[-1]["frame"]-8
        gpu = {r["gpu_frame"]: r["gpu_ms"] for r in rows
               if low <= r["gpu_frame"] <= high and r["gpu_ms"] > 0}
        wall = [r["wall_ms"] for r in measured]
        cpu = [r["total_ms"]-r["bgfx_frame"] for r in measured]
        row = dict(name=scenario["name"], settings=scenario, frames=len(measured),
                   fps=1000/statistics.mean(wall), wall_p50_ms=statistics.median(wall),
                   wall_p95_ms=percentile(wall, .95), wall_p99_ms=percentile(wall, .99),
                   cpu_p50_ms=statistics.median(cpu), cpu_p95_ms=percentile(cpu, .95),
                   gpu_p50_ms=statistics.median(gpu.values()) if gpu else None,
                   gpu_p95_ms=percentile(list(gpu.values()), .95), gpu_samples=len(gpu),
                   draws=statistics.median(r["draws"] for r in measured),
                   width=statistics.median(r["width"] for r in measured),
                   height=statistics.median(r["height"] for r in measured),
                   stages={k: statistics.median(r[k] for r in measured) for k in
                       ("events", "pending_io", "state_update", "imgui_build", "scene_state", "view_setup",
                        "hover_pick", "submit_scene", "submit_helpers", "imgui_render", "bgfx_frame", "render_cpu_ms")})
        result.append(row)
    return dict(metadata=meta, scenarios=result)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--models", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--rounds", type=int, default=2)
    p.add_argument("--model-filter", default="")
    p.add_argument("--scenario-filter", default="")
    p.add_argument("--warmup", type=float, default=1.5)
    p.add_argument("--seconds", type=float, default=3)
    p.add_argument("--captures", action="store_true")
    p.add_argument("--suite", choices=("core", "extra", "all"), default="core")
    args = p.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    models = sorted(m for m in args.models.resolve().glob("*.obj") if args.model_filter in m.name)
    choices = [s for s in scenarios(args.suite) if not args.scenario_filter or s["name"] in args.scenario_filter.split(",")]
    assert choices and models
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    for iteration in range(args.rounds):
        for model in models if iteration % 2 == 0 else reversed(models):
            ordered = choices.copy()
            random.Random(1907 + iteration).shuffle(ordered)
            stem = output / f"{model.stem}-{iteration+1}"
            config = dict(output=str(stem), binary=str(args.binary.resolve()), scenarios=ordered, warmup_seconds=args.warmup,
                          measure_seconds=args.seconds, min_frames=30, captures=args.captures)
            config_path = stem.with_suffix(".config.json")
            config_path.write_text(json.dumps(config, indent=2))
            print(f"START round={iteration+1} model={model.name}", flush=True)
            start = time.perf_counter()
            with stem.with_suffix(".log").open("w") as log:
                proc = subprocess.run([str(args.binary.resolve()), "--file", str(model), "--instance", "render-fps-probe",
                                       "--log-level", "warn", "--log-file", str(stem.with_suffix(".app.log"))], cwd=output, startupinfo=startup,
                                      env=dict(os.environ, WOBY_RENDER_FPS_CONFIG=str(config_path)),
                                      stdout=log, stderr=log, timeout=1800)
            if proc.returncode:
                raise RuntimeError(f"Viewer failed {proc.returncode}: {stem}.log")
            summary = summarize_run(stem)
            summary.update(model=model.name, round=iteration+1, process_seconds=time.perf_counter()-start)
            stem.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2))
            with (output / "runs.jsonl").open("a") as stream:
                stream.write(json.dumps(summary)+"\n")
            for row in summary["scenarios"]:
                print(f"  {row['name']:24} {row['fps']:7.1f} FPS CPU {row['cpu_p50_ms']:7.2f} ms "
                      f"GPU {row['gpu_p50_ms']} ms hover {row['stages']['hover_pick']:.2f} ms", flush=True)
            print(f"DONE {summary['process_seconds']:.1f}s", flush=True)


if __name__ == "__main__":
    main()
