"""Run the full viewer serially; marker-only runs never build the point BVH."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("fps", HERE.parent / "render_fps/run.py")
FPS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FPS)


def scenarios(suite="core", validation=False):
    core = [dict(case="fit", pointer="sweep"), dict(case="far", pointer="center", distance_factor=4),
            dict(case="near", pointer="center", distance_factor=.5), dict(case="zoom", pointer="center", motion="zoom"),
            dict(case="orbit", pointer="center", motion="orbit"), dict(case="pan", pointer="center", motion="pan")]
    extra = [dict(case="solid", pointer="sweep", solid=True), dict(case="alpha", pointer="sweep", opacity=.4),
             dict(case="single", pointer="sweep", msaa=False), dict(case="large", pointer="center", point_size=16),
             dict(case="1080p", pointer="sweep", width=1920, height=1080),
             dict(case="pane", pointer="sweep", pane=True), dict(case="hidden", pointer="center", visible=False),
             dict(case="zero", pointer="center", opacity=0)]
    cases = core if suite == "core" else extra if suite == "extra" else core + extra
    choices = []
    for case in cases:
        if not validation and case["case"] in ("hidden","zero"):
            continue
        modes = ["id_async"] if validation else ["none", "id_resident", "id_async"]
        if not validation and case in extra:
            modes = ["none","id_async"]
        if not validation and case["case"] == "fit":
            modes += ["routed", "id_buffer", "id_immediate"]
        for mode in modes:
            choices.append(dict(solid=False, vertices=True) | case | {"picker": mode, "name": f"{case['case']}_{mode}"})
    return choices


def execute(binary, model, output, choices, iteration, validation=False, seconds=3, captures=False, fixture=False, overrides=None):
    stem = output / f"{model.stem}-{iteration+1}"
    config = dict(output=str(stem), binary=str(binary), scenarios=choices, validate=validation,
                  warmup_seconds=0 if validation else 1.5, warmup_frames=2 if validation else 20,
                  measure_seconds=0 if validation else seconds, min_frames=8 if validation else 30, captures=captures, fixture=fixture)
    config.update(overrides or {})
    path = stem.with_suffix(".config.json")
    path.write_text(json.dumps(config, indent=2))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    print(f"START {model.name} round={iteration+1} validate={validation}", flush=True)
    started = time.perf_counter()
    with stem.with_suffix(".log").open("w") as stream:
        result = subprocess.run([str(binary), "--file", str(model), "--instance", "marker-experiment", "--log-level", "warn",
                                 "--log-file", str(stem.with_suffix(".app.log"))], cwd=output, startupinfo=startup,
                                env=dict(os.environ, WOBY_RENDER_FPS_CONFIG=str(path)), stdout=stream, stderr=stream, timeout=3600)
    if result.returncode:
        raise RuntimeError(f"Viewer exit {result.returncode}: {stem}.log")
    summary = FPS.summarize_run(stem)
    summary.update(model=model.name, round=iteration+1, process_seconds=time.perf_counter()-started,
                   marker_path=str(stem.with_suffix(".marker.json")))
    marker = json.loads(stem.with_suffix(".marker.json").read_text())
    if marker["metadata"]["spatial_index_build_ms"] != 0:
        raise RuntimeError("Unexpected index construction")
    if validation and not marker["metadata"]["validated_requests"]:
        raise RuntimeError("No GPU results validated")
    stem.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2))
    with (output / "runs.jsonl").open("a") as stream:
        stream.write(json.dumps(summary) + "\n")
    for row in summary["scenarios"]:
        print(f"  {row['name']:24} {row['fps']:8.1f} FPS", flush=True)
    print(f"DONE {summary['process_seconds']:.1f}s; validated={marker['metadata']['validated_requests']}", flush=True)
    return marker


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--models", type=Path, default=Path("D:/temp/obj_tests"))
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--rounds", type=int, default=2)
    p.add_argument("--model-filter", default="")
    p.add_argument("--scenario-filter", default="")
    p.add_argument("--suite", choices=("core", "extra", "all"), default="core")
    p.add_argument("--validate", action="store_true")
    p.add_argument("--captures", action="store_true")
    p.add_argument("--wide", action="store_true", help="Use the original 16-byte ID/depth/draw attachment")
    p.add_argument("--seconds", type=float, default=3)
    args = p.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    models = sorted(m for m in args.models.resolve().glob("*.obj") if args.model_filter in m.name)
    choices = scenarios(args.suite, args.validate)
    if args.scenario_filter:
        choices = [s for s in choices if s["name"] in args.scenario_filter.split(",")]
    assert models and choices
    for iteration in range(args.rounds):
        for model in models if iteration % 2 == 0 else reversed(models):
            ordered = choices.copy()
            if not args.validate:
                random.Random(3701 + iteration).shuffle(ordered)
                ordered.insert(0, dict(name="prime", case="prime", picker="id_async", vertices=True, solid=False, pointer="center"))
            execute(args.binary.resolve(), model, output, ordered, iteration, args.validate, args.seconds, args.captures,
                    overrides=dict(compact_ids=not args.wide))


if __name__ == "__main__":
    main()
